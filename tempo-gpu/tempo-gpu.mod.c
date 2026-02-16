#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x7e2232fb, "ioread32" },
	{ 0xa61fd7aa, "__check_object_size" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0x54c9c6cd, "pci_enable_device" },
	{ 0xfad8f384, "iowrite32" },
	{ 0x43867a4d, "pci_iomap" },
	{ 0xe0418251, "eventfd_ctx_put" },
	{ 0x49733ad6, "queue_work_on" },
	{ 0x334e7b26, "memdup_user" },
	{ 0x1e05893e, "pci_dev_put" },
	{ 0x051e71da, "pci_get_device" },
	{ 0xacac6336, "memcpy_fromio" },
	{ 0x535f4f5f, "hrtimer_init" },
	{ 0x3835fa62, "remap_pfn_range" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0x474f3b3e, "seq_lseek" },
	{ 0xe1e1f979, "_raw_spin_lock_irqsave" },
	{ 0xd272d446, "__fentry__" },
	{ 0xdd6830c7, "sysfs_emit" },
	{ 0xa4a97a44, "pci_read_config_dword" },
	{ 0x5a844b26, "__x86_indirect_thunk_rax" },
	{ 0xcb5bc4b5, "memcpy_toio" },
	{ 0xe8213e80, "_printk" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x166ee2ee, "put_device" },
	{ 0xd710adbf, "__kmalloc_large_noprof" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0x0688a4fa, "cc_mkdec" },
	{ 0xa59da3c0, "down_write" },
	{ 0xa59da3c0, "up_write" },
	{ 0xd09b06f5, "kstrtoint" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xf46d5bf3, "mutex_lock" },
	{ 0xac4c6ce9, "dma_alloc_attrs" },
	{ 0x1ddc4b5a, "debugfs_remove" },
	{ 0x2f4b655e, "pci_read_config_word" },
	{ 0x9a58b2bc, "vfio_unregister_group_dev" },
	{ 0x3c0300ea, "eventfd_ctx_fdget" },
	{ 0x173ec8da, "sscanf" },
	{ 0xc1e6c71e, "__mutex_init" },
	{ 0x81a1a811, "_raw_spin_unlock_irqrestore" },
	{ 0x84a69724, "pci_iounmap" },
	{ 0x27683a56, "memset" },
	{ 0x5fa07cc0, "hrtimer_start_range_ns" },
	{ 0xabbb06fa, "pci_set_master" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x092a35a2, "_copy_to_user" },
	{ 0xfa2b6e99, "seq_read" },
	{ 0x82fd7238, "__ubsan_handle_shift_out_of_bounds" },
	{ 0xba98e203, "dma_free_attrs" },
	{ 0xbdc59e2e, "vfio_register_group_dev" },
	{ 0xf46d5bf3, "mutex_unlock" },
	{ 0xcbae5412, "__const_udelay" },
	{ 0x23f25c0a, "__dynamic_pr_debug" },
	{ 0x9264bfeb, "__kmalloc_cache_noprof" },
	{ 0x97acb853, "ktime_get" },
	{ 0x2d88a3ab, "cancel_work_sync" },
	{ 0xa90b70a1, "seq_printf" },
	{ 0x51656d76, "debugfs_create_file_full" },
	{ 0x36a36ab1, "hrtimer_cancel" },
	{ 0xb1ad3f2f, "boot_cpu_data" },
	{ 0x7900f37d, "single_release" },
	{ 0x49fc4616, "hrtimer_forward" },
	{ 0x84f07bf7, "cachemode2protval" },
	{ 0xe4de56b4, "__ubsan_handle_load_invalid_value" },
	{ 0x65dc3f69, "single_open" },
	{ 0x2d648fc0, "debugfs_create_dir" },
	{ 0xaf7502fc, "mdev_register_parent" },
	{ 0x6b1b3ec4, "_vfio_alloc_device" },
	{ 0xd30dea77, "kmalloc_caches" },
	{ 0xaef1f20d, "system_wq" },
	{ 0x6d9023d2, "mdev_unregister_parent" },
	{ 0x0f2a95ae, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x7e2232fb,
	0xa61fd7aa,
	0x092a35a2,
	0x54c9c6cd,
	0xfad8f384,
	0x43867a4d,
	0xe0418251,
	0x49733ad6,
	0x334e7b26,
	0x1e05893e,
	0x051e71da,
	0xacac6336,
	0x535f4f5f,
	0x3835fa62,
	0xcb8b6ec6,
	0x474f3b3e,
	0xe1e1f979,
	0xd272d446,
	0xdd6830c7,
	0xa4a97a44,
	0x5a844b26,
	0xcb5bc4b5,
	0xe8213e80,
	0xd272d446,
	0x166ee2ee,
	0xd710adbf,
	0x90a48d82,
	0x0688a4fa,
	0xa59da3c0,
	0xa59da3c0,
	0xd09b06f5,
	0xbd03ed67,
	0xf46d5bf3,
	0xac4c6ce9,
	0x1ddc4b5a,
	0x2f4b655e,
	0x9a58b2bc,
	0x3c0300ea,
	0x173ec8da,
	0xc1e6c71e,
	0x81a1a811,
	0x84a69724,
	0x27683a56,
	0x5fa07cc0,
	0xabbb06fa,
	0xd272d446,
	0x092a35a2,
	0xfa2b6e99,
	0x82fd7238,
	0xba98e203,
	0xbdc59e2e,
	0xf46d5bf3,
	0xcbae5412,
	0x23f25c0a,
	0x9264bfeb,
	0x97acb853,
	0x2d88a3ab,
	0xa90b70a1,
	0x51656d76,
	0x36a36ab1,
	0xb1ad3f2f,
	0x7900f37d,
	0x49fc4616,
	0x84f07bf7,
	0xe4de56b4,
	0x65dc3f69,
	0x2d648fc0,
	0xaf7502fc,
	0x6b1b3ec4,
	0xd30dea77,
	0xaef1f20d,
	0x6d9023d2,
	0x0f2a95ae,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"ioread32\0"
	"__check_object_size\0"
	"_copy_from_user\0"
	"pci_enable_device\0"
	"iowrite32\0"
	"pci_iomap\0"
	"eventfd_ctx_put\0"
	"queue_work_on\0"
	"memdup_user\0"
	"pci_dev_put\0"
	"pci_get_device\0"
	"memcpy_fromio\0"
	"hrtimer_init\0"
	"remap_pfn_range\0"
	"kfree\0"
	"seq_lseek\0"
	"_raw_spin_lock_irqsave\0"
	"__fentry__\0"
	"sysfs_emit\0"
	"pci_read_config_dword\0"
	"__x86_indirect_thunk_rax\0"
	"memcpy_toio\0"
	"_printk\0"
	"__stack_chk_fail\0"
	"put_device\0"
	"__kmalloc_large_noprof\0"
	"__ubsan_handle_out_of_bounds\0"
	"cc_mkdec\0"
	"down_write\0"
	"up_write\0"
	"kstrtoint\0"
	"random_kmalloc_seed\0"
	"mutex_lock\0"
	"dma_alloc_attrs\0"
	"debugfs_remove\0"
	"pci_read_config_word\0"
	"vfio_unregister_group_dev\0"
	"eventfd_ctx_fdget\0"
	"sscanf\0"
	"__mutex_init\0"
	"_raw_spin_unlock_irqrestore\0"
	"pci_iounmap\0"
	"memset\0"
	"hrtimer_start_range_ns\0"
	"pci_set_master\0"
	"__x86_return_thunk\0"
	"_copy_to_user\0"
	"seq_read\0"
	"__ubsan_handle_shift_out_of_bounds\0"
	"dma_free_attrs\0"
	"vfio_register_group_dev\0"
	"mutex_unlock\0"
	"__const_udelay\0"
	"__dynamic_pr_debug\0"
	"__kmalloc_cache_noprof\0"
	"ktime_get\0"
	"cancel_work_sync\0"
	"seq_printf\0"
	"debugfs_create_file_full\0"
	"hrtimer_cancel\0"
	"boot_cpu_data\0"
	"single_release\0"
	"hrtimer_forward\0"
	"cachemode2protval\0"
	"__ubsan_handle_load_invalid_value\0"
	"single_open\0"
	"debugfs_create_dir\0"
	"mdev_register_parent\0"
	"_vfio_alloc_device\0"
	"kmalloc_caches\0"
	"system_wq\0"
	"mdev_unregister_parent\0"
	"module_layout\0"
;

MODULE_INFO(depends, "vfio,mdev");


MODULE_INFO(srcversion, "B0F055253F2D4C46B9E976E");
