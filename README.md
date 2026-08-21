# apolloOS

A hobby operating system kernel written in C and x86_64 assembly.

## Features

- **Kernel**: Custom x86_64 kernel with multiboot2 support (GRUB), Long Mode
- **Memory Management**: Physical (PMM) and Virtual (VMM) memory managers, kernel heap (kheap)
- **Paging**: Page fault handler with demand paging, copy-on-write (COW) fork and stack growth
- **Process Management**: Preemptive scheduler with FPU/SSE save/restore (fxsave/fxrstor),
  signals (`rt_sigaction`/`rt_sigreturn`/`kill`), fork/execve/waitpid, job control
- **Syscalls**: 46 syscalls — fs, dir ops, pipes, mmap/brk, signals, sockets, job control
- **Filesystems**: VFS layer with FAT32 (read-write, VFAT LFN), ramfs, procfs, devfs,
  write-back block cache, pipes
- **Firmware loader**: `request_firmware()` backed by cpio initrd → `/lib/firmware` → built-in table
- **Interrupts**: IDT with IST for double fault, `request_irq()` with IRQ threading,
  PCI MSI enable/disable + MSI-X vector masking
- **Bottom halves**: Workqueues + timerwheel (delayed work)
- **DMA API**: ops-table based (`dma_alloc_coherent`, `map_sg`, sync helpers, dma masks)
- **Network stack**: RTL8139 driver, ARP/IP/ICMP/UDP, socket syscalls, loopback interface
- **DRM subsystem**: native DRM core (`/dev/dri/card0`, ioctl dispatch), GEM buffer objects
  with VRAM carveout, dma-fence/dma-resv with boot-time selftests
- **GPU drivers**:
  - Polaris/R500 probe + modeset 1920×1080×32 + test pattern
  - `apollo_drv.c`: validation driver exercising the full DRM/GEM/fence/ring/IRQ APIs
    (baseline for the future amdgpu compatibility layer)
- **Drivers**: framebuffer/console, keyboard (ABNT2), mouse, serial, PIT timer, RTC, ATA/PATA,
  PCI enumeration, TTY line discipline, minimal window manager
- **vgpu**: forked QEMU with emulated NVIDIA/AMD/Intel GPUs for testing (see below)

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

On every boot the kernel runs its selftests: GEM BO create/write/read/unmap
(`drm_gem_test`) and dma-fence/resv/timeline tests (`dma_test_run_all`).

## Project Structure

```
apolloOS/
├── boot/            # Multiboot2 bootloader (assembly)
├── kernel/
│   ├── cpu/         # GDT, IDT, interrupts, syscall entry (asm + C)
│   ├── drivers/     # Device drivers
│   │   ├── drm/     # Native DRM: core/device, GEM, dma-fence, dma-resv, selftests
│   │   ├── gpu/     # GPU drivers (apollo validation driver, polaris)
│   │   ├── net/     # RTL8139 ethernet
│   │   └── ...      # framebuffer, keyboard, mouse, serial, timer, rtc, tty, wm
│   ├── fs/          # VFS, FAT32, ramfs, procfs, devfs, pipe, bcache, firmware
│   ├── include/     # Kernel headers (+ drm/, net/)
│   ├── kernel/      # Core kernel (DMA API, workqueue/timerwheel, panic)
│   ├── lib/         # string helpers
│   ├── loader/      # ELF64 loader
│   ├── mem/         # Memory management (PMM, VMM w/ #PF+COW+demand paging, kheap)
│   ├── net/         # Network stack (arp, ip, icmp, udp, loopback, core)
│   ├── pci/         # PCI enumeration + MSI/MSI-X
│   ├── kernel.c     # Kernel entry point (init sequence)
│   ├── syscall.c    # System call handlers (46 syscalls)
│   └── task.c       # Preemptive scheduler, signals, fork/wait
├── scripts/         # Build helper scripts (disk image creation)
├── vgpu/            # Forked QEMU with custom GPU device models + test scripts
├── grub.cfg         # GRUB config (multiboot2)
├── linker.ld        # Linker script
└── Makefile
```

## Related Projects

- **vgpu**: Separate GPU virtualization project at https://github.com/asimplestray/vgpu

## License

MIT License - see [LICENSE](LICENSE) for details.
