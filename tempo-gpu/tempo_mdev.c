// SPDX-License-Identifier: GPL-2.0
/*
 * Tempo GPU: VFIO Mediated Device Driver
 *
 * Module entry point, PCI setup, mdev lifecycle (create/remove),
 * and VFIO device operations (ioctl/read/write/mmap).
 */

#include "tempo_gpu.h"

#include <linux/mdev.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/eventfd.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/log2.h>
#include <linux/slab.h>

/*
 * VFIO-PCI encodes region offsets such that the region index lives in the
 * upper bits of the file offset. This is not part of the uapi, but matching
 * vfio-pci keeps QEMU happy and mirrors the standard VFIO PCI layout.
 */
#define TEMPO_VFIO_PCI_OFFSET_SHIFT 40
#define TEMPO_VFIO_PCI_OFFSET_MASK  (((u64)1 << TEMPO_VFIO_PCI_OFFSET_SHIFT) - 1)
#define TEMPO_VFIO_PCI_INDEX_TO_OFFSET(i) ((u64)(i) << TEMPO_VFIO_PCI_OFFSET_SHIFT)
#define TEMPO_VFIO_PCI_OFFSET_TO_INDEX(o) ((u32)((o) >> TEMPO_VFIO_PCI_OFFSET_SHIFT))

/*
 * We want extra per-mdev state (PCI config space emulation, IRQ eventfd).
 * We cannot change struct tempo_vm (in the public header), so we wrap it
 * in a private extension structure. Note: vfio_alloc_device() requires
 * that the vfio_device is at offset 0 of the allocated structure; this
 * is satisfied because:
 *   ext.vm is first field (offset 0)
 *   tempo_vm.vfio_dev is first field inside tempo_vm (offset 0)
 */
struct tempo_vm_ext {
	struct tempo_vm vm;

	/* Emulated PCI config space (we advertise PCIe config size when possible) */
	u8 cfg[PCI_CFG_SPACE_EXP_SIZE];

	/* Simple MSI trigger eventfd (kernel -> userspace) */
	struct eventfd_ctx *msi_trigger;
	struct mutex irq_lock;
};

struct tempo_gpu_state *g_tempo;

/* BAR1 CPU aperture (host mapping of VRAM window) */
static void __iomem *tempo_bar1;
static resource_size_t tempo_bar1_phys;
static resource_size_t tempo_bar1_size;

static inline struct tempo_vm_ext *tempo_vm_to_ext(struct tempo_vm *vm)
{
	return container_of(vm, struct tempo_vm_ext, vm);
}

static inline struct tempo_vm_ext *tempo_vfio_to_ext(struct vfio_device *vdev)
{
	return container_of(vdev, struct tempo_vm_ext, vm.vfio_dev);
}

static size_t tempo_pci_config_size(void)
{
	/*
	 * If the physical GPU is PCIe, expose 4K config space; otherwise 256B.
	 * We still keep a 4K buffer, but report only what makes sense.
	 */
	if (g_tempo && g_tempo->pdev && pci_is_pcie(g_tempo->pdev))
		return PCI_CFG_SPACE_EXP_SIZE;

	return PCI_CFG_SPACE_SIZE;
}

static u32 tempo_pci_bar_mask(u64 size)
{
	u64 s;

	if (!size)
		return 0;

	/* BAR sizing rules are power-of-two; round up if caller gives non-pow2 */
	s = roundup_pow_of_two(size);

	/* At minimum, BAR alignment is 16 bytes, but page-aligned is fine */
	if (s < PAGE_SIZE)
		s = PAGE_SIZE;

	/* Mask includes only address bits; low 4 bits reserved for MEM BAR */
	return (u32)(~(s - 1) & 0xFFFFFFF0u);
}

static void tempo_cfg_init(struct tempo_vm_ext *tv)
{
	struct pci_dev *pdev = g_tempo ? g_tempo->pdev : NULL;
	u32 class_rev = 0;
	u16 subven = 0, subdev = 0;
	__le16 le16;
	__le32 le32;

	memset(tv->cfg, 0, sizeof(tv->cfg));

	/* Vendor / Device */
	le16 = cpu_to_le16(pdev ? pdev->vendor : 0x10de);
	memcpy(&tv->cfg[PCI_VENDOR_ID], &le16, sizeof(le16));
	le16 = cpu_to_le16(pdev ? pdev->device : 0x0000);
	memcpy(&tv->cfg[PCI_DEVICE_ID], &le16, sizeof(le16));

	/* Class / Revision (pass through from the physical GPU when available) */
	if (pdev)
		pci_read_config_dword(pdev, PCI_CLASS_REVISION, &class_rev);
	le32 = cpu_to_le32(class_rev);
	memcpy(&tv->cfg[PCI_CLASS_REVISION], &le32, sizeof(le32));

	/* Header type: normal */
	tv->cfg[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL;

	/* Subsystem IDs (best-effort passthrough) */
	if (pdev) {
		pci_read_config_word(pdev, PCI_SUBSYSTEM_VENDOR_ID, &subven);
		pci_read_config_word(pdev, PCI_SUBSYSTEM_ID, &subdev);
	}
	le16 = cpu_to_le16(subven);
	memcpy(&tv->cfg[PCI_SUBSYSTEM_VENDOR_ID], &le16, sizeof(le16));
	le16 = cpu_to_le16(subdev);
	memcpy(&tv->cfg[PCI_SUBSYSTEM_ID], &le16, sizeof(le16));

	/*
	 * BAR registers are emulated via reads/writes to cfg[].
	 * We do NOT hardcode the size mask into config space; instead we
	 * emulate standard BAR sizing behavior: write 0xFFFFFFFF, read mask.
	 */

	/* Interrupt pin (arbitrary non-zero so guests don't assume "no IRQ") */
	tv->cfg[PCI_INTERRUPT_PIN] = 0x01;
}

static ssize_t tempo_cfg_read(struct tempo_vm_ext *tv, char __user *buf,
			      size_t count, loff_t *ppos)
{
	size_t cfgsz = tempo_pci_config_size();
	loff_t pos = *ppos;
	size_t n;

	if (pos < 0)
		return -EINVAL;
	if (pos >= cfgsz)
		return 0;

	n = min_t(size_t, count, cfgsz - pos);

	/*
	 * Special-case aligned dword reads of BAR0/BAR1 for size probing.
	 * (Good enough for QEMU/PCI core behavior.)
	 */
	if (n == 4 && (pos == PCI_BASE_ADDRESS_0 || pos == PCI_BASE_ADDRESS_1)) {
		u32 cur;
		u32 mask;
		u32 val;
		__le32 le;

		memcpy(&le, &tv->cfg[pos], sizeof(le));
		cur = le32_to_cpu(le);

		if (cur == 0xFFFFFFFFu || cur == 0xFFFFFFF0u) {
			/* Return size mask */
			if (pos == PCI_BASE_ADDRESS_0)
				mask = tempo_pci_bar_mask(g_tempo ? g_tempo->bar0_size : 0);
			else
				mask = tempo_pci_bar_mask(tv->vm.vram_size);

			val = mask; /* MEM BAR, 32-bit, prefetchable=0 */
		} else {
			val = cur;
		}

		le = cpu_to_le32(val);
		if (copy_to_user(buf, &le, sizeof(le)))
			return -EFAULT;

		*ppos += 4;
		return 4;
	}

	if (copy_to_user(buf, &tv->cfg[pos], n))
		return -EFAULT;

	*ppos += n;
	return n;
}

static ssize_t tempo_cfg_write(struct tempo_vm_ext *tv, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	size_t cfgsz = tempo_pci_config_size();
	loff_t pos = *ppos;
	size_t n;

	if (pos < 0)
		return -EINVAL;
	if (pos >= cfgsz)
		return -EINVAL;

	n = min_t(size_t, count, cfgsz - pos);

	/*
	 * Allow QEMU to manage the config space image.
	 * We do not enforce RO fields here; QEMU's vfio-pci device model expects
	 * to write and maintain the presented config state.
	 */
	if (n == 4 && (pos == PCI_BASE_ADDRESS_0 || pos == PCI_BASE_ADDRESS_1)) {
		u32 val;
		__le32 le;

		if (copy_from_user(&val, buf, sizeof(val)))
			return -EFAULT;

		le = cpu_to_le32(val);
		memcpy(&tv->cfg[pos], &le, sizeof(le));

		*ppos += 4;
		return 4;
	}

	if (copy_from_user(&tv->cfg[pos], buf, n))
		return -EFAULT;

	*ppos += n;
	return n;
}

static ssize_t tempo_bar1_read(struct tempo_vm *vm, char __user *buf,
			       size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	u64 start;
	size_t done = 0;
	void *tmp;

	if (!tempo_bar1)
		return -ENODEV;
	if (pos < 0)
		return -EINVAL;

	/* Bounds within the VM's BAR1 region */
	if ((u64)pos >= vm->vram_size)
		return 0;

	count = min_t(size_t, count, vm->vram_size - (u64)pos);

	/* Bounds within the host BAR1 window */
	start = vm->vram_base + (u64)pos;
	if (start + count > tempo_bar1_size)
		return -EINVAL;

	tmp = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	while (done < count) {
		size_t chunk = min_t(size_t, PAGE_SIZE, count - done);

		memcpy_fromio(tmp, tempo_bar1 + start + done, chunk);
		if (copy_to_user(buf + done, tmp, chunk)) {
			kfree(tmp);
			return -EFAULT;
		}
		done += chunk;
	}

	kfree(tmp);
	*ppos += done;
	return done;
}

static ssize_t tempo_bar1_write(struct tempo_vm *vm, const char __user *buf,
				size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	u64 start;
	size_t done = 0;
	void *tmp;

	if (!tempo_bar1)
		return -ENODEV;
	if (pos < 0)
		return -EINVAL;

	if ((u64)pos >= vm->vram_size)
		return -EINVAL;

	count = min_t(size_t, count, vm->vram_size - (u64)pos);

	start = vm->vram_base + (u64)pos;
	if (start + count > tempo_bar1_size)
		return -EINVAL;

	tmp = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	while (done < count) {
		size_t chunk = min_t(size_t, PAGE_SIZE, count - done);

		if (copy_from_user(tmp, buf + done, chunk)) {
			kfree(tmp);
			return -EFAULT;
		}
		memcpy_toio(tempo_bar1 + start + done, tmp, chunk);
		done += chunk;
	}

	kfree(tmp);
	*ppos += done;
	return done;
}

/* ──────────────────────────────────────────────────────────────────
 * VFIO Device Operations
 * ────────────────────────────────────────────────────────────────── */

static int tempo_vfio_open_device(struct vfio_device *vdev)
{
	struct tempo_vm_ext *tv = tempo_vfio_to_ext(vdev);
	struct tempo_vm *vm = &tv->vm;

	pr_info(TEMPO_DRIVER_NAME ": VM %d opened by QEMU (uuid %pUb)\n",
		vm->vm_id, &vm->uuid);

	return 0;
}

static void tempo_vfio_close_device(struct vfio_device *vdev)
{
	struct tempo_vm_ext *tv = tempo_vfio_to_ext(vdev);
	struct tempo_vm *vm = &tv->vm;
	unsigned long flags;

	if (!g_tempo)
		return;

	/* Deschedule this VM if it's currently running */
	if (READ_ONCE(g_tempo->active_vm_id) == vm->vm_id) {
		spin_lock_irqsave(&g_tempo->sched_lock, flags);
		vm->state = VM_STATE_IDLE;
		g_tempo->active_vm_id = -1;
		spin_unlock_irqrestore(&g_tempo->sched_lock, flags);

		tempo_runlist_build_and_submit_empty(g_tempo);
	}

	pr_info(TEMPO_DRIVER_NAME ": VM %d closed\n", vm->vm_id);
}

static long tempo_vfio_set_irqs(struct tempo_vm_ext *tv, void __user *uarg)
{
	struct vfio_irq_set hdr;
	struct vfio_irq_set *kset = NULL;
	size_t argsz, minsz;
	u32 data_type, action;
	long ret = 0;

	if (copy_from_user(&hdr, uarg, sizeof(hdr)))
		return -EFAULT;

	if (hdr.argsz < sizeof(hdr))
		return -EINVAL;

	argsz = hdr.argsz;
	kset = memdup_user(uarg, argsz);
	if (IS_ERR(kset))
		return PTR_ERR(kset);

	/* We only support MSI trigger eventfd (kernel -> userspace) */
	if (kset->index != VFIO_PCI_MSI_IRQ_INDEX) {
		ret = -ENOTTY;
		goto out;
	}

	data_type = kset->flags & VFIO_IRQ_SET_DATA_TYPE_MASK;
	action = kset->flags & VFIO_IRQ_SET_ACTION_TYPE_MASK;

	if (action != VFIO_IRQ_SET_ACTION_TRIGGER) {
		ret = -ENOTTY;
		goto out;
	}

	/* Disable entire MSI index */
	if (data_type == VFIO_IRQ_SET_DATA_NONE && kset->count == 0) {
		mutex_lock(&tv->irq_lock);
		if (tv->msi_trigger) {
			eventfd_ctx_put(tv->msi_trigger);
			tv->msi_trigger = NULL;
		}
		mutex_unlock(&tv->irq_lock);
		ret = 0;
		goto out;
	}

	if (data_type != VFIO_IRQ_SET_DATA_EVENTFD) {
		ret = -ENOTTY;
		goto out;
	}

	/* We only expose a single MSI vector */
	if (kset->start != 0 || kset->count != 1) {
		ret = -EINVAL;
		goto out;
	}

	minsz = sizeof(*kset) + sizeof(s32) * kset->count;
	if (argsz < minsz) {
		ret = -EINVAL;
		goto out;
	}

	/* Bind/Unbind eventfd */
	{
		s32 fd;
		struct eventfd_ctx *ctx;

		memcpy(&fd, kset->data, sizeof(fd));

		mutex_lock(&tv->irq_lock);

		if (fd == -1) {
			/* Deassign */
			if (tv->msi_trigger) {
				eventfd_ctx_put(tv->msi_trigger);
				tv->msi_trigger = NULL;
			}
			mutex_unlock(&tv->irq_lock);
			ret = 0;
			goto out;
		}

		ctx = eventfd_ctx_fdget(fd);
		if (IS_ERR(ctx)) {
			mutex_unlock(&tv->irq_lock);
			ret = PTR_ERR(ctx);
			goto out;
		}

		if (tv->msi_trigger)
			eventfd_ctx_put(tv->msi_trigger);

		tv->msi_trigger = ctx;

		mutex_unlock(&tv->irq_lock);
		ret = 0;
	}

out:
	kfree(kset);
	return ret;
}

static long tempo_vfio_ioctl(struct vfio_device *vdev, unsigned int cmd,
			     unsigned long arg)
{
	struct tempo_vm_ext *tv = tempo_vfio_to_ext(vdev);
	struct tempo_vm *vm = &tv->vm;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO: {
		struct vfio_device_info info;

		if (copy_from_user(&info, uarg, sizeof(info)))
			return -EFAULT;

		if (info.argsz < sizeof(info))
			return -EINVAL;

		info.flags = VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET;
		info.num_regions = VFIO_PCI_NUM_REGIONS;
		info.num_irqs = VFIO_PCI_NUM_IRQS;
		info.cap_offset = 0;

		if (copy_to_user(uarg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	case VFIO_DEVICE_GET_REGION_INFO: {
		struct vfio_region_info reg;
		u32 index;

		if (copy_from_user(&reg, uarg, sizeof(reg)))
			return -EFAULT;

		if (reg.argsz < sizeof(reg))
			return -EINVAL;

		index = reg.index;
		memset(&reg, 0, sizeof(reg));
		reg.argsz = sizeof(reg);
		reg.index = index;
		reg.offset = TEMPO_VFIO_PCI_INDEX_TO_OFFSET(index);

		switch (index) {
		case VFIO_PCI_CONFIG_REGION_INDEX:
			reg.flags = VFIO_REGION_INFO_FLAG_READ |
				    VFIO_REGION_INFO_FLAG_WRITE;
			reg.size = tempo_pci_config_size();
			break;

		case VFIO_PCI_BAR0_REGION_INDEX:
			/* BAR0: MMIO registers — trapped and emulated */
			reg.flags = VFIO_REGION_INFO_FLAG_READ |
				    VFIO_REGION_INFO_FLAG_WRITE;
			reg.size = g_tempo ? g_tempo->bar0_size : 0;
			break;

		case VFIO_PCI_BAR1_REGION_INDEX:
			/* BAR1: VRAM aperture — mapped to VM's partition */
			reg.flags = VFIO_REGION_INFO_FLAG_READ |
				    VFIO_REGION_INFO_FLAG_WRITE |
				    VFIO_REGION_INFO_FLAG_MMAP;
			reg.size = vm->vram_size;
			break;

		default:
			reg.size = 0;
			reg.flags = 0;
			break;
		}

		if (copy_to_user(uarg, &reg, sizeof(reg)))
			return -EFAULT;
		return 0;
	}

	case VFIO_DEVICE_GET_IRQ_INFO: {
		struct vfio_irq_info irq;
		u32 index;

		if (copy_from_user(&irq, uarg, sizeof(irq)))
			return -EFAULT;

		if (irq.argsz < sizeof(irq))
			return -EINVAL;

		index = irq.index;
		memset(&irq, 0, sizeof(irq));
		irq.argsz = sizeof(irq);
		irq.index = index;

		if (index == VFIO_PCI_MSI_IRQ_INDEX) {
			irq.flags = VFIO_IRQ_INFO_EVENTFD;
			irq.count = 1;
		} else {
			irq.flags = 0;
			irq.count = 0;
		}

		if (copy_to_user(uarg, &irq, sizeof(irq)))
			return -EFAULT;
		return 0;
	}

	case VFIO_DEVICE_SET_IRQS:
		return tempo_vfio_set_irqs(tv, uarg);

	case VFIO_DEVICE_RESET:
		pr_info(TEMPO_DRIVER_NAME ": VM %d reset\n", vm->vm_id);
		vm->state = VM_STATE_IDLE;
		return 0;

	default:
		return -ENOTTY;
	}
}

static ssize_t tempo_vfio_read(struct vfio_device *vdev, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct tempo_vm_ext *tv = tempo_vfio_to_ext(vdev);
	struct tempo_vm *vm = &tv->vm;
	u64 off = *ppos;
	u32 index = TEMPO_VFIO_PCI_OFFSET_TO_INDEX(off);
	loff_t pos = (loff_t)(off & TEMPO_VFIO_PCI_OFFSET_MASK);
	ssize_t ret;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		ret = tempo_cfg_read(tv, buf, count, &pos);
		break;

	case VFIO_PCI_BAR0_REGION_INDEX:
		ret = tempo_mmio_read(g_tempo, vm, buf, count, &pos);
		break;

	case VFIO_PCI_BAR1_REGION_INDEX:
		ret = tempo_bar1_read(vm, buf, count, &pos);
		break;

	default:
		return -EINVAL;
	}

	if (ret > 0)
		*ppos = (loff_t)(TEMPO_VFIO_PCI_INDEX_TO_OFFSET(index) + (u64)pos);

	return ret;
}

static ssize_t tempo_vfio_write(struct vfio_device *vdev, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct tempo_vm_ext *tv = tempo_vfio_to_ext(vdev);
	struct tempo_vm *vm = &tv->vm;
	u64 off = *ppos;
	u32 index = TEMPO_VFIO_PCI_OFFSET_TO_INDEX(off);
	loff_t pos = (loff_t)(off & TEMPO_VFIO_PCI_OFFSET_MASK);
	ssize_t ret;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		ret = tempo_cfg_write(tv, buf, count, &pos);
		break;

	case VFIO_PCI_BAR0_REGION_INDEX:
		ret = tempo_mmio_write(g_tempo, vm, buf, count, &pos);
		break;

	case VFIO_PCI_BAR1_REGION_INDEX:
		ret = tempo_bar1_write(vm, buf, count, &pos);
		break;

	default:
		return -EINVAL;
	}

	if (ret > 0)
		*ppos = (loff_t)(TEMPO_VFIO_PCI_INDEX_TO_OFFSET(index) + (u64)pos);

	return ret;
}

static int tempo_vfio_mmap(struct vfio_device *vdev, struct vm_area_struct *vma)
{
	struct tempo_vm_ext *tv = tempo_vfio_to_ext(vdev);
	struct tempo_vm *vm = &tv->vm;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	u32 index = TEMPO_VFIO_PCI_OFFSET_TO_INDEX(off);
	u64 pos = off & TEMPO_VFIO_PCI_OFFSET_MASK;
	unsigned long size = vma->vm_end - vma->vm_start;
	resource_size_t phys;

	if (index != VFIO_PCI_BAR1_REGION_INDEX)
		return -EINVAL;

	if (!tempo_bar1_phys || !tempo_bar1_size)
		return -ENODEV;

	/* Bounds within the VM's BAR1 region */
	if (pos + size > vm->vram_size)
		return -EINVAL;

	/* Bounds within the host BAR1 window */
	if (vm->vram_base + pos + size > tempo_bar1_size)
		return -EINVAL;

	phys = tempo_bar1_phys + vm->vram_base + pos;

	// vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
    vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return io_remap_pfn_range(vma, vma->vm_start,
				  phys >> PAGE_SHIFT,
				  size, vma->vm_page_prot);
}

static void tempo_vfio_release(struct vfio_device *vdev)
{
	struct tempo_vm_ext *tv = tempo_vfio_to_ext(vdev);

	/* Best-effort cleanup */
	mutex_lock(&tv->irq_lock);
	if (tv->msi_trigger) {
		eventfd_ctx_put(tv->msi_trigger);
		tv->msi_trigger = NULL;
	}
	mutex_unlock(&tv->irq_lock);

	vfio_put_device(vdev);
}

static const struct vfio_device_ops tempo_vfio_ops = {
	.name         = TEMPO_DRIVER_NAME,
	.release      = tempo_vfio_release,
	.open_device  = tempo_vfio_open_device,
	.close_device = tempo_vfio_close_device,
	.ioctl        = tempo_vfio_ioctl,
	.read         = tempo_vfio_read,
	.write        = tempo_vfio_write,
	.mmap         = tempo_vfio_mmap,
};

/* ──────────────────────────────────────────────────────────────────
 * MDEV Sysfs Type Support (new-style mdev_type + callbacks)
 * ────────────────────────────────────────────────────────────────── */

static unsigned int tempo_mdev_get_available(struct mdev_type *mtype)
{
	unsigned long flags;
	unsigned int used;

	if (!g_tempo)
		return 0;

	spin_lock_irqsave(&g_tempo->sched_lock, flags);
	used = g_tempo->num_vms;
	spin_unlock_irqrestore(&g_tempo->sched_lock, flags);

	if (used >= TEMPO_MAX_VMS)
		return 0;

	return TEMPO_MAX_VMS - used;
}

static ssize_t tempo_mdev_show_description(struct mdev_type *mtype, char *buf)
{
	return sysfs_emit(buf,
			  "Full GPU access via temporal sharing. "
			  "Each VM gets exclusive GPU during its time slice.\n");
}

static struct mdev_type tempo_mdev_type = {
	.sysfs_name  = "tempo-1g",
	.pretty_name = "Tempo GPU Full Share (temporal)",
};

static struct mdev_type *tempo_mdev_types[] = {
	&tempo_mdev_type,
};

static int tempo_mdev_probe(struct mdev_device *mdev)
{
	struct tempo_vm_ext *tv;
	struct tempo_vm *vm;
	int vm_id = -1;
	int tsg_id;
	int ret;
	unsigned long flags;
	int i;

	if (!g_tempo)
		return -ENODEV;

	/* Allocate VM struct + VFIO device in one managed allocation */
	tv = vfio_alloc_device(tempo_vm_ext, vm.vfio_dev, &mdev->dev, &tempo_vfio_ops);
	if (IS_ERR(tv))
		return PTR_ERR(tv);

	vm = &tv->vm;
	mutex_init(&tv->irq_lock);
	tv->msi_trigger = NULL;

	/* Initialize emulated config space */
	tempo_cfg_init(tv);

	/* Allocate a VM slot */
	spin_lock_irqsave(&g_tempo->sched_lock, flags);
	if (g_tempo->num_vms >= TEMPO_MAX_VMS) {
		spin_unlock_irqrestore(&g_tempo->sched_lock, flags);
		ret = -ENOSPC;
		goto err_put;
	}

	for (i = 0; i < TEMPO_MAX_VMS; i++) {
		if (!g_tempo->vms[i]) {
			vm_id = i;
			g_tempo->vms[i] = vm; /* reserve slot */
			g_tempo->num_vms++;
			break;
		}
	}
	spin_unlock_irqrestore(&g_tempo->sched_lock, flags);

	if (vm_id < 0) {
		ret = -ENOSPC;
		goto err_put;
	}

	/* Initialize VM fields */
	vm->vm_id    = vm_id;
	vm->active   = true;
	vm->state    = VM_STATE_IDLE;
	vm->priority = PRIO_STANDARD;
	guid_copy(&vm->uuid, &mdev->uuid);

	/* Allocate TSG + VRAM partition */
	tsg_id = tempo_tsg_alloc(g_tempo, vm_id);
	if (tsg_id < 0) {
		ret = tsg_id;
		goto err_unreserve;
	}
	vm->tsg_id = (u32)tsg_id;

	ret = tempo_vram_alloc_partition(g_tempo, vm);
	if (ret)
		goto err_free_tsg;

	/* Register with VFIO core */
	ret = vfio_register_group_dev(&vm->vfio_dev);
	if (ret)
		goto err_free_vram;

	dev_set_drvdata(&mdev->dev, vm);

	pr_info(TEMPO_DRIVER_NAME ": created VM %d (TSG %u, VRAM %llu MB)\n",
		vm_id, vm->tsg_id, vm->vram_size >> 20);

	return 0;

err_free_vram:
	tempo_vram_free_partition(g_tempo, vm);
err_free_tsg:
	tempo_tsg_free(g_tempo, vm->tsg_id);
err_unreserve:
	spin_lock_irqsave(&g_tempo->sched_lock, flags);
	if (vm_id >= 0 && vm_id < TEMPO_MAX_VMS && g_tempo->vms[vm_id] == vm) {
		g_tempo->vms[vm_id] = NULL;
		if (g_tempo->num_vms > 0)
			g_tempo->num_vms--;
	}
	spin_unlock_irqrestore(&g_tempo->sched_lock, flags);
err_put:
	vfio_put_device(&vm->vfio_dev);
	return ret;
}

static void tempo_mdev_remove(struct mdev_device *mdev)
{
	struct tempo_vm *vm = dev_get_drvdata(&mdev->dev);
	struct tempo_vm_ext *tv;
	unsigned long flags;

	if (!vm || !g_tempo)
		return;

	tv = tempo_vm_to_ext(vm);

	/*
	 * Unregister first: blocks until all users close (QEMU releases FD).
	 * After this returns, no VFIO file ops are in-flight.
	 */
	vfio_unregister_group_dev(&vm->vfio_dev);

	/* Deschedule if active */
	spin_lock_irqsave(&g_tempo->sched_lock, flags);
	if (g_tempo->active_vm_id == vm->vm_id)
		g_tempo->active_vm_id = -1;

	vm->state  = VM_STATE_IDLE;
	vm->active = false;

	/* Remove from VM table */
	if (vm->vm_id >= 0 && vm->vm_id < TEMPO_MAX_VMS &&
	    g_tempo->vms[vm->vm_id] == vm) {
		g_tempo->vms[vm->vm_id] = NULL;
		if (g_tempo->num_vms > 0)
			g_tempo->num_vms--;
	}
	spin_unlock_irqrestore(&g_tempo->sched_lock, flags);

	tempo_runlist_build_and_submit_empty(g_tempo);

	/* Release IRQ eventfd */
	mutex_lock(&tv->irq_lock);
	if (tv->msi_trigger) {
		eventfd_ctx_put(tv->msi_trigger);
		tv->msi_trigger = NULL;
	}
	mutex_unlock(&tv->irq_lock);

	/* Release resources */
	tempo_tsg_free(g_tempo, vm->tsg_id);
	tempo_vram_free_partition(g_tempo, vm);

	dev_set_drvdata(&mdev->dev, NULL);

	pr_info(TEMPO_DRIVER_NAME ": removed VM %d\n", vm->vm_id);

	/* Drop allocation reference (triggers tempo_vfio_release -> vfio_free_device) */
	vfio_put_device(&vm->vfio_dev);
}

static struct mdev_driver tempo_mdev_driver = {
	.device_api       = VFIO_DEVICE_API_PCI_STRING,
	.max_instances    = TEMPO_MAX_VMS,
	.probe            = tempo_mdev_probe,
	.remove           = tempo_mdev_remove,
	.get_available    = tempo_mdev_get_available,
	.show_description = tempo_mdev_show_description,
	.driver = {
		.name  = TEMPO_DRIVER_NAME,
		.owner = THIS_MODULE,
	},
};

static struct mdev_parent tempo_mdev_parent;

/* ──────────────────────────────────────────────────────────────────
 * PCI / BAR Setup
 * ────────────────────────────────────────────────────────────────── */

static int tempo_map_bars(struct tempo_gpu_state *g)
{
	int ret;

	ret = pci_enable_device(g->pdev);
	if (ret) {
		pr_err(TEMPO_DRIVER_NAME ": pci_enable_device failed: %d\n", ret);
		return ret;
	}
	pci_set_master(g->pdev);

	/* BAR0 (registers) */
	g->bar0_size = pci_resource_len(g->pdev, 0);
	if (!g->bar0_size) {
		pr_err(TEMPO_DRIVER_NAME ": BAR0 has zero length\n");
		return -ENODEV;
	}

	g->bar0 = pci_iomap(g->pdev, 0, 0);
	if (!g->bar0) {
		pr_err(TEMPO_DRIVER_NAME ": failed to map BAR0\n");
		return -ENOMEM;
	}

	/* BAR1 (VRAM aperture) - optional but required for mmap/read/write of BAR1 */
	tempo_bar1_size = pci_resource_len(g->pdev, 1);
	tempo_bar1_phys = pci_resource_start(g->pdev, 1);
	if (tempo_bar1_size) {
		tempo_bar1 = pci_iomap(g->pdev, 1, 0);
		if (!tempo_bar1) {
			pr_warn(TEMPO_DRIVER_NAME ": failed to map BAR1 (VRAM aperture)\n");
			tempo_bar1_size = 0;
			tempo_bar1_phys = 0;
		}
	}

	/* Read chip identification */
	g->boot0   = tempo_rd32(g, NV_PMC_BOOT_0);
	g->chip_id = (g->boot0 >> 20) & 0x1FF;

	pr_info(TEMPO_DRIVER_NAME ": BAR0 mapped at %p (size %llu KB)\n",
		g->bar0, (u64)g->bar0_size >> 10);
	if (tempo_bar1)
		pr_info(TEMPO_DRIVER_NAME ": BAR1 mapped at %p (size %llu KB)\n",
			tempo_bar1, (u64)tempo_bar1_size >> 10);

	pr_info(TEMPO_DRIVER_NAME ": GPU BOOT0=0x%08x chip_id=0x%03x\n",
		g->boot0, g->chip_id);

	return 0;
}

static void tempo_unmap_bars(struct tempo_gpu_state *g)
{
	if (tempo_bar1) {
		pci_iounmap(g->pdev, tempo_bar1);
		tempo_bar1 = NULL;
		tempo_bar1_phys = 0;
		tempo_bar1_size = 0;
	}

	if (g->bar0) {
		pci_iounmap(g->pdev, g->bar0);
		g->bar0 = NULL;
	}
}

/* ──────────────────────────────────────────────────────────────────
 * Module Init / Exit
 * ────────────────────────────────────────────────────────────────── */

static int __init tempo_gpu_init(void)
{
	struct pci_dev *pdev = NULL;
	struct pci_dev *next = NULL;
	int ret;

	pr_info(TEMPO_DRIVER_NAME ": Tempo GPU Temporal Scheduler loading...\n");

	/* Find the first NVIDIA GPU (vendor 0x10DE) */
	pdev = pci_get_device(0x10DE, PCI_ANY_ID, NULL);
	while (pdev) {
		/* Accept only 3D controllers (class 0x0302) or VGA (0x0300) */
		if ((pdev->class >> 8) == 0x0302 || (pdev->class >> 8) == 0x0300)
			break;

		next = pci_get_device(0x10DE, PCI_ANY_ID, pdev);
		pci_dev_put(pdev);
		pdev = next;
	}

	if (!pdev) {
		pr_err(TEMPO_DRIVER_NAME ": no NVIDIA GPU found\n");
		return -ENODEV;
	}

	/* Allocate global state */
	g_tempo = kzalloc(sizeof(*g_tempo), GFP_KERNEL);
	if (!g_tempo) {
		pci_dev_put(pdev);
		return -ENOMEM;
	}

	g_tempo->pdev = pdev;
	g_tempo->active_vm_id = -1;
	spin_lock_init(&g_tempo->sched_lock);
	INIT_WORK(&g_tempo->sched_work, NULL); /* tempo_sched_init() may override */

	/* Map GPU MMIO registers (and BAR1 aperture if available) */
	ret = tempo_map_bars(g_tempo);
	if (ret)
		goto fail_free;

	/* Initialize subsystems */
	ret = tempo_tsg_init(g_tempo);
	if (ret)
		goto fail_unmap;

	ret = tempo_runlist_init(g_tempo);
	if (ret)
		goto fail_tsg;

	ret = tempo_vram_init(g_tempo);
	if (ret)
		goto fail_runlist;

	tempo_idle_init(g_tempo);
	tempo_sched_init(g_tempo);

	/* Register mdev parent on the GPU PCI device */
	ret = mdev_register_parent(&tempo_mdev_parent,
				   &pdev->dev,
				   &tempo_mdev_driver,
				   tempo_mdev_types,
				   ARRAY_SIZE(tempo_mdev_types));
	if (ret) {
		pr_err(TEMPO_DRIVER_NAME ": mdev_register_parent failed: %d\n", ret);
		goto fail_sched;
	}

	/* Debugfs */
	tempo_debugfs_init(g_tempo);

	pr_info(TEMPO_DRIVER_NAME ": initialized on %s (chip 0x%03x)\n",
		pci_name(pdev), g_tempo->chip_id);
	return 0;

fail_sched:
	tempo_sched_exit(g_tempo);
	tempo_idle_exit(g_tempo);
	tempo_vram_exit(g_tempo);
fail_runlist:
	tempo_runlist_exit(g_tempo);
fail_tsg:
	tempo_tsg_exit(g_tempo);
fail_unmap:
	tempo_unmap_bars(g_tempo);
fail_free:
	pci_dev_put(pdev);
	kfree(g_tempo);
	g_tempo = NULL;
	return ret;
}

static void __exit tempo_gpu_exit(void)
{
	if (!g_tempo)
		return;

	tempo_debugfs_exit(g_tempo);
	mdev_unregister_parent(&tempo_mdev_parent);

	tempo_sched_exit(g_tempo);
	tempo_idle_exit(g_tempo);
	tempo_vram_exit(g_tempo);
	tempo_runlist_exit(g_tempo);
	tempo_tsg_exit(g_tempo);

	tempo_unmap_bars(g_tempo);

	pci_dev_put(g_tempo->pdev);
	kfree(g_tempo);
	g_tempo = NULL;

	pr_info(TEMPO_DRIVER_NAME ": unloaded\n");
}

module_init(tempo_gpu_init);
module_exit(tempo_gpu_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Tempo GPU Project");
MODULE_DESCRIPTION("Tempo GPU: Workload-Aware Temporal GPU Scheduler for VMs");
MODULE_VERSION("0.1");

