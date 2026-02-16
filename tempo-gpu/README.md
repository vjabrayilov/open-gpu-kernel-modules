```
tempo-gpu/
├── Makefile
├── tempo_gpu.h             # Core data structures
├── tempo_mdev.c            # VFIO mdev driver (lifecycle, MMIO interception)
├── tempo_scheduler.c       # Scheduling policies (round-robin → kernel-boundary → bubble)
├── tempo_runlist.c         # Runlist construction and submission
├── tempo_tsg.c             # TSG and channel management (wraps RM API)
├── tempo_vram.c            # VRAM partitioning and page table management
├── tempo_idle.c            # Idle detection (GET/PUT monitoring)
├── tempo_debugfs.c         # Debug/measurement interface
└── tempo_mmio.c            # MMIO emulation (register virtualization)
```


