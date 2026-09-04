# apolloOS

AmigaOS-inspired hobby OS kernel (x86_64, C + asm). Filosofia Exec/Intuition (leve, orientado a mensagens, cobre/blitter via DRM/KMS) implementada sobre base monolítica moderna com compat POSIX para porte de drivers Linux.

> **Nota de modelo:** o código atual é majoritariamente Unix/Linux-like por necessidade de porte (`task_struct`+`fork(COW)`, VFS fd-table, `drm_device` — ver `kernel/task.c:182`, `kernel/fs/vfs.c:57`, `kernel/include/drm/drm_device.h:69`), mas a *intenção arquitetural* é Amiga-like: `workqueue`+`drm_sched` como `MsgPort/PutMsg`, `wm` como `intuition.library` minimal, `apollo_drv.c` como `library .base`. Roadmap para `Exec` puro está em `PRE_AMDGPU_TASKS.md`.

## Modelo (Amiga-like sobre base Unix)

**Exec-like (alvo) → implementação atual:**
- `Exec Tasks/Ports/Messages` → `task_struct` preemptivo `kernel/task.c:182` + `runqueue_lock`+`need_resched` + `work_struct`/`drm_sched` `kernel/kernel/workqueue.c:27`/`kernel/drivers/drm/drm_sched.c:252` como `PutMsg`/`WaitPort` (passivo hoje, precisa `kworker` `PRE_AMDGPU_TASKS.md:P1.3`)
- `intuition.library Screens/Windows` → `wm` stacked `kernel/drivers/wm.c:46` (`window_t {x,y,w,h,focused}`, `Tokyo Night` wallpaper), `framebuffer.c:56` `fb_put_pixel` linear XRGB como `RastPort`/`copper` via `DCE` `kernel/drivers/gpu/amd/amdgpu/amdgpu_mode.c:1`
- `dos.library/Handler` → `VFS` `kernel/fs/vfs.c:57` `vfs_node_t` ops + `ramfs` `kernel/fs/ramfs.c:11` + `FAT32 VFAT` `kernel/fs/fat32.c:1` + `pipe` circular 4K `kernel/fs/pipe.c:10` como `DOS Packet` (fd-table `kernel/task.c:182` `files->fd_array[256]` é POSIX, migração para `MsgPort` pendente)
- `graphics.library` → `DRM` `kernel/include/drm/drm_device.h:69` (`registers/vram/ring/fence/kms.mode`) + `GEM` `kernel/include/drm/drm_gem.h:22` + `dma_fence/resv` como blitter semáforos

## Features (estado v0.2-Alpha `KERNEL_TODO.md:3` + Amiga extentions `c32c427`)

- **Kernel**: x86_64 `multiboot2` `boot/boot.asm:1` → Long Mode `linker.ld:4` `PHDRS R E/RW` (híbrido BIOS+EFI via `grub-mkrescue` `Makefile:66`)
- **Memory**: `PMM` bitmap `kernel/mem/pmm.c:58` (fix `PRE_AMDGPU_TASKS.md:P0` protege `initrd`), `VMM` 4-level `kernel/mem/vmm.c:15`, `kheap`, `#PF` `kernel/mem/vmm.c:330` `demand+COW` `VMM_FLAG_COW` + `stack growth`
- **Exec/Tasks**: preemptivo `schedule()` `kernel/task.c:255` `fxsave_area[512]` `CR0.TS`, `fork(COW)` `kernel/task.c:108` `vmm_clone_user_pml4`, `wait_chldexit` `ZOMBIE` + `wait_queue` `kernel/include/wait.h:17` (Exec `Signal` → `POSIX sigaction` `kernel/task.c:523` ainda) + **Amiga**: `MsgPort` `kernel/ipc/msgport.c:1` `AOS_CreatePort/PutMsg/GetMsg/WaitPort/ReplyMsg` `AOS_Assign` `kernel/fs/assign.c:1` `Sys: Ram: Work: C: Devs:` (`/proc/assigns`+`/proc/ports`)
- **Syscalls**: 53 `AOS_*` `kernel/include/syscall.h:109` (`AOS_Exit/SpawnTask/AllocMem/FindTask/Yield/Delay` + `AOS_Assign/CreatePort`) `NR 53` compat `SYS_*` alias, dispatch `kernel/syscall.c:34`
- **Filesystems**: `VFS`+`RamFS`+`FAT32 RW VFAT LFN`+`procfs`+`devfs`+`bcache`+`pipe` (`Assign` expande `Work:docs/readme` `kernel/fs/vfs.c:102`)
- **Firmware**: `request_firmware()` `kernel/fs/firmware.c:1` cpio initrd `module2 /boot/fw.cpio` `grub.cfg:10` → `/lib/firmware` → built-in
- **Interrupts**: `IDT+IST double fault` `kernel/cpu/idt.c:1` + `GDT/TSS` `kernel/cpu/gdt.c:98`, `request_irq` threaded, `PCI MSI/MSI-X` `kernel/pci/pci.c:60/151` (mask OK, alloc dinâmica pendente `PRE_AMDGPU_TASKS.md:P1.2`)
- **Bottom halves**: `workqueue+timerwheel 512` `kernel/kernel/workqueue.c:8` (passivo, `flush_workqueue:123` manual)
- **DMA**: ops-table `kernel/kernel/dma.c:16` `dma_alloc_coherent/map_sg` identity (IOMMU DMAR stub pendente `PRE_AMDGPU_TASKS.md:P1.1`)
- **Net**: `RTL8139` `kernel/drivers/net/rtl8139.c:1` + `ARP/IP/ICMP/UDP` `kernel/net/` + `BSD sockets` `AOS_Socket/Bind`
- **DRM**: `drm_device.c:565` `/dev/dri/card0`, `GEM` carveout 1MB `kernel/drivers/drm/drm_gem.c:1`, `dma-fence/resv` `dma_test_run_all` + `dma_buf/PRIME` + `drm_sched entity/RR/timeout` + `atomic KMS rollback` + `compat/linux_*.h` — selftests no boot
- **GPU**: `amd/amdgpu v0.1.0 MINIMAL` `kernel/include/amdgpu.h:4` (`CHIP_POLARIS10/11/12/NAVI22`) `rmmio 0xFFFF800600000000` `VRAM 0xFFFF800700000000` + `DCE 1920x1080@60` `amdgpu_mode.c` + `GFX ring 4096dw WRITE_DATA/FENCE` `amdgpu_gfx.c:75` + `thermal 85/95°C` + `DC` `dc_core.c/dce_resource.c/dcn_resource.c` `HPD`/`MST`/`flip` — `vgpu amd-rx480` `screenshot6_amdgpu_rx480.png`; `apollo_drv.c:60` validação universal Polaris/Kepler/Xe
- **Drivers**: `framebuffer` GOP, `keyboard ABNT2`, `mouse`, `serial`, `PIT 100Hz` `kernel/drivers/timer.c:1`, `RTC`, `ATA`, `TTY termios` `kernel/include/tty.h:55` (canon `SIGINT` `kernel/drivers/tty.c:233`)
- **vgpu**: QEMU fork `vgpu/` `nvidia-gt730/gk208/gm107/gp104/tu102` `amd gfx8/gfx10` `intel` — `vgpu/scripts/run-test.sh`

## Building

Requirements:
- `nasm` (assembler)
- `gcc`/`ld` (x86_64 host toolchain, freestanding)
- `grub-mkrescue` and `grub-file` (for ISO creation / multiboot2 verification)
- `qemu-system-x86_64` (for testing)

```bash
make        # builds apolloos.iso
make run    # boot in QEMU (SDL window)
make run-vnc # boot in QEMU, VNC on localhost:5900
make clean
```

## Testing with vGPU emulation

The `vgpu/` directory contains a forked QEMU tree with custom GPU device models
(`nvidia-gt730`, gk208/gm107/gp104/tu102, amd gfx8/gfx10, intel) used to test the
kernel's PCI/DRM/GPU paths against well-known hardware:

```bash
vgpu/scripts/run-test.sh apolloos.iso 2G nvidia-gt730
# other devices: nvidia-gtx750ti, nvidia-gtx1080, nvidia-rtx2080,
#                amd-rx480, amd-rx6700xt, intel-arc-a770
```

On every boot: `drm_gem_test`+`dma_test_run_all`+`assign_test`/`msgport_test` (`c32c427` `Sys:/C:/Work:` + `CreatePort` `PutMsg` PASS) + `compat/dma_buf/drm_sched/drm_atomic` + `amdgpu_{mem,display,gfx,dc}_selftest` em `amd-rx480` (`kernel/kernel.c:128`). `PRE_AMDGPU_TASKS.md` blockers Fase 5.

## Project Structure

```
apolloOS/
├── boot/            # Multiboot2 (boot.asm:1) + stack 16K, 4GB huge pages
├── kernel/
│   ├── cpu/         # GDT gdt.c:98 (TSS IST1) + IDT + interrupts.asm + syscall.asm
│   ├── drivers/
│   │   ├── drm/     # drm_device.c:565 /dev/dri/card0, drm_gem.c, dma_fence.c/resv.c, dma_buf.c, drm_sched.c, drm_atomic.c
│   │   ├── gpu/
│   │   │   ├── amd/amdgpu/ # amdgpu_device.c/vram_mgr/mode/gfx/fw/thermal/pattern + dc/dc_core.c/dce_resource.c/dcn_resource.c
│   │   │   └── apollo/     # apollo_drv.c:60 universal validação (Polaris/Kepler/Xe) + polaris.c legacy
│   │   ├── net/     # rtl8139.c
│   │   └── ...      # framebuffer.c (GOP), keyboard ABNT2, mouse, serial 0x3F8, timer PIT 100Hz, rtc, ata, tty.c:233 SIGINT, wm.c intuition-like
│   ├── fs/          # vfs.c:57 (Assign expand) + assign.c:1 Sys: + ramfs.c:11 + fat32.c:1 VFAT + procfs (+/proc/assigns/ports) + devfs + pipe.c:10 4K + bcache + firmware.c cpio
│   ├── ipc/         # msgport.c:1 AOS_CreatePort/PutMsg/GetMsg/WaitPort/ReplyMsg 128B
│   ├── include/     # + drm/ + compat/linux_*.h + assign.h/msgport.h + syscall.h:109 AOS_* 53 + amdgpu.h:50
│   ├── kernel/      # dma.c:16 ops-table + workqueue.c:27 timerwheel 512 + panic
│   ├── lib/         # string
│   ├── loader/      # elf.c
│   ├── mem/         # pmm.c:58 bitmap (fix initrd) + vmm.c:15 4-level + kheap
│   ├── net/         # arp, ip, icmp, udp, loopback, core
│   ├── pci/         # pci.c:60 MSI/151 MSI-X + BAR WC vmm.c:100
│   ├── kernel.c:40  # init: serial→fb→idt/gdt→pmm/vmm/kheap→workqueue→vfs/procfs/devfs/drm→firmware→fat32→task→pci→timer/rtc/kbd/tty/syscall(AOS_53)→net→sti→selftests(assign/msgport/dma/compat)→amdgpu→idle
│   ├── syscall.c:34 # AOS_* 53 traps
│   └── task.c:255   # scheduler prio decay + fxsave + aos_signal AOS_*
├── scripts/         # make_fw_initrd.sh (8 blobs polaris10 389632B)
├── vgpu/            # fork QEMU + vgpu_dce.c scanout + run-test.sh
├── PRE_AMDGPU_TASKS.md # P0-P3 antes do amdgpu 6.6 (EFI PF, IOMMU, MSI-X, kworker, SDMA/DOOM)
├── GPU_PORTING_STRATEGY.md / GPU_PORTING_TASKS.md / KERNEL_TODO.md
├── grub.cfg:1       # multiboot2 /boot/apolloos.bin + module2 /boot/fw.cpio + all_video/gfxterm
├── linker.ld:4      # 1M + PHDRS text R E / data RW
└── Makefile:66      # grub-mkrescue híbrido + grub-file multiboot2
```

## Related Projects

- **vgpu**: Separate GPU virtualization project at https://github.com/asimplestray/vgpu

## License

MIT License - see [LICENSE](LICENSE) for details.
