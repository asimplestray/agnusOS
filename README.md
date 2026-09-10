# AgnusOS

**Amiga spirit, modern muscle.**  
x86_64 monolithic kernel (C + asm) — AmigaOS-like API (Exec/DOS/Intuition) over a Linux-driven substrate (MMU, preempt, DRM/KMS, PCI, VFS).

> **Why "Agnus"?** The Agnus chip (Address Generator Unit) was the heart of the Amiga — DMA, Chip RAM, Copper/Blitter sync. This kernel is the modern equivalent: the central coordinator moving data between CPU, GPU, devices, and memory.

---

## Architecture: Hybrid by Design

| Layer | Implementation | Rationale |
|-------|----------------|-----------|
| **Memory** | 4-level paging, per-process PML4, demand paging, COW stack growth, PMM bitmap | GPU/IOMMU needs isolation; `MEMF_CHIP/FAST/PUBLIC` pools map to VRAM/RAM |
| **Tasks** | Preemptive round-robin, FPU lazy-restore (#NM), `CreateTask` (no `fork`) | Amiga tasks are independent; `fork`/`COW` removed |
| **Signals** | 32-bit bitmask (`sig_recv`/`sig_wait`/`sig_except`), no handlers/frames | Pure `Signal`/`Wait` model — `Wait(mask)` blocks until any bit arrives |
| **IPC** | `MsgPort` + `PutMsg`/`GetMsg`/`WaitPort`/`ReplyMsg`, `Assign` (volumes) | Native Amiga message ports; `kworker` pending for async |
| **Files** | `BPTR` handles, `FileInfoBlock`, `dos_open/read/write/seek/examine/exnext` | DOS layer over VFS (RamFS, FAT32 VFAT/LFN, procfs, devfs, pipe) |
| **GPU/DRM** | `drm_device` + GEM + dma-fence/resv + `drm_sched` + `amdgpu` DC (DCE/DCN) | `graphics.library` future; `apollo_drv.c` validates Polaris/Kepler/Xe |
| **Net** | RTL8139 + ARP/IP/ICMP/UDP + BSD sockets (`bsdsocket.library`) | Amiga-style socket API |
| **Boot** | Multiboot2 → Long Mode, Limine 7.12 (BIOS+UEFI), GRUB fallback | 3.1 MB ISO |

---

## Syscall Surface (61 traps, `AOS_*` namespace)

```
AOS_Exit           AOS_SpawnTask      AOS_Read           AOS_Write
AOS_Open           AOS_Close          AOS_Wait           AOS_LoadSeg
AOS_AllocMem       AOS_FreeMem        AOS_DoIO           AOS_FindTask
AOS_Yield          AOS_Delay          AOS_GetSysTime     AOS_Signal
AOS_SetSignal      AOS_ReturnSignal   AOS_SendSignal     AOS_Pipe
AOS_Seek           AOS_Examine        AOS_ExamineDir     AOS_Flush
AOS_CreateDir      AOS_DeleteDir      AOS_DeleteFile     AOS_CurrentDir
AOS_LockCWD        AOS_Rename         AOS_Assign
AOS_CreatePort     AOS_DeletePort     AOS_PutMsg         AOS_GetMsg
AOS_WaitPort       AOS_ReplyMsg
AOS_Socket         AOS_Bind           AOS_Send           AOS_Recv
AOS_CloseSocket    AOS_Select         AOS_SetSockOpt     AOS_GetSockOpt
AOS_GetSocketAddr  AOS_SocketIOCtl    AOS_SocketBaseTags AOS_SendTo
AOS_RecvFrom
AOS_CreatePool     AOS_DeletePool     AOS_AllocPooled    AOS_FreePooled
AOS_PoolAvail
```

No `SYS_*`, `O_*`, POSIX signal numbers, `fd_array`, `pgid`, `fork`/`COW` machinery.

---

## Project Structure

```
agnusOS/
├── boot/                    # Multiboot2 entry (boot.asm) + 16K stack
├── kernel/
│   ├── cpu/                 # GDT (TSS IST1), IDT, syscall/interrupts asm
│   ├── drivers/
│   │   ├── drm/             # drm_device, GEM, dma-fence/resv/buf, drm_sched, atomic
│   │   ├── gpu/
│   │   │   ├── amd/amdgpu/  # device, vram_mgr, mode, gfx, fw, thermal, DC (DCE/DCN)
│   │   │   └── apollo/      # apollo_drv (universal GPU validation), polaris legacy
│   │   ├── net/             # rtl8139
│   │   └── ...              # fb (GOP), kbd (ABNT2), mouse, serial, PIT 100Hz, RTC, ATA, TTY, WM
│   ├── exec/                # ExecBase (SysBase), library system, MEMF alloc, **Memory Pools**
│   ├── fs/                  # VFS, RamFS, FAT32, procfs, devfs, pipe, bcache, firmware cpio, assign
│   ├── ipc/                 # MsgPort (128B messages)
│   ├── kernel/              # DMA ops, workqueue+timerwheel, panic
│   ├── loader/              # ELF loader
│   ├── mem/                 # PMM bitmap, VMM 4-level, kheap
│   ├── net/                 # ARP, IP, ICMP, UDP, loopback, core
│   ├── pci/                 # MSI/MSI-X, BAR WC, IOMMU VT-d identity
│   ├── kernel.c             # Init sequence → idle
│   ├── syscall.c            # **67 AOS_* traps**
│   └── task.c               # Scheduler, signal bitmask, task mgmt
├── scripts/                 # make_fw_initrd.sh (Polaris10 blobs)
├── vgpu/                    # QEMU fork with GPU device models (NVIDIA/AMD/Intel)
├── limine/                  # Vendored Limine 7.12 (BOOTX64.EFI, limine-bios.sys, etc.)
├── limine.cfg               # Limine PROTOCOL=multiboot2
├── grub.cfg                 # GRUB fallback
├── linker.ld                # 1M base, PHDRS text R E / data RW
└── Makefile                 # Limine xorriso+bios-install (default), GRUB fallback
```

---

## Building

```bash
# Requirements
nasm gcc ld xorriso mformat mcopy limine(7.12+) qemu-system-x86_64 OVMF

make              # Limine BIOS+UEFI ISO (3.1 MB) — default
make grub-iso     # GRUB fallback (32 MB)
make run          # QEMU SDL (BIOS)
make clean
```

UEFI manual test:
```bash
qemu-system-x86_64 -m 512M -cdrom agnusos.iso \
  -drive if=pflash,format=raw,unit=0,file=/usr/share/OVMF/x64/OVMF_CODE.4m.fd,readonly=on \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -display sdl -vga std -serial stdio
```

---

## vGPU Testing (GPU CI)

```bash
vgpu/scripts/run-test.sh agnusos.iso 2G nvidia-gt730
# Devices: nvidia-gt730, nvidia-gtx750ti, nvidia-gtx1080, nvidia-rtx2080
#          amd-rx480, amd-rx6700xt, intel-arc-a770
```

On every boot: `drm_gem_test` + `dma_test_run_all` + `assign_test`/`msgport_test` + `compat/dma_buf/drm_sched/drm_atomic` + `amdgpu_{mem,display,gfx,dc}_selftest` on `amd-rx480`.

---

## Current Status (v0.3)

| Subsystem | Status |
|-----------|--------|
| Memory (PMM/VMM/kheap) | ✅ Stable |
| Scheduler (preempt, FPU lazy) | ✅ Stable |
| Signal bitmask (32-bit) | ✅ Complete |
| **Wait blocking real (timeout)** | ✅ **Complete** |
| MsgPort / Assign | ✅ Complete |
| DOS layer (BPTR, FileInfoBlock) | ✅ Complete + wired to syscalls |
| VFS + RamFS + FAT32 VFAT/LFN | ✅ RW |
| procfs / devfs / pipe / bcache | ✅ |
| PCI + MSI/MSI-X + IOMMU VT-d | ✅ |
| DRM/GEM/dma-fence/resv/drm_sched | ✅ |
| amdgpu (Polaris/Navi22, DC) | ⚠️ MINIMAL — GFX ring, mode, thermal |
| RTL8139 + IPv4/UDP + bsdsocket | ✅ |
| TTY (canon, termios, ABNT2) | ✅ |
| **Memory Pools (MEMF_CHIP/FAST/PUBLIC)** | ✅ **Complete** |
| dogin shell | ⚠️ Basic — pipes/redirect/env/Run pending |

---

## Roadmap (Next)

1. **IORequest async** (`SendIO`/`WaitIO`/`AbortIO` via MsgPort + kworker)
2. **dogin** — pipes, redirect, env vars, `Run`, `Execute`, `If`/`While`
3. **Intuition** — Layers (damage-rectangle), Screens, Windows, Gadgets
4. **Datatypes** — ELF/PNG/IFF/text loaders
5. **amdgpu** — full GFX/compute, SDMA, formally verified command submission

---

## Related

- **vgpu** — GPU virtualization fork: https://github.com/asimplestray/vgpu

---

## License

MIT — see [LICENSE](LICENSE)