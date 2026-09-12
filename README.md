# AgnusOS

**Amiga spirit, modern muscle.**  
x86_64 monolithic kernel (C + asm) — AmigaOS-like API (Exec/DOS/Intuition) over a Linux-driven substrate (MMU, preempt, DRM/KMS, PCI, VFS).

> **Why "Agnus"?** The Agnus chip (Address Generator Unit) was the heart of the Amiga — DMA, Chip RAM, Copper/Blitter sync. This kernel is the modern equivalent: the central coordinator moving data between CPU, GPU, devices, and memory.

---

## Architecture: Hybrid by Design

| Layer | Implementation | Rationale |
|-------|----------------|-----------|
| **Memory** | 4-level paging, per-process PML4, demand paging (stack-growth only), PMM bitmap | GPU/IOMMU needs isolation; `MEMF_CHIP/FAST/PUBLIC` pools map to VRAM/RAM (bump allocator, `FreePooled` no-op — see Status) |
| **Tasks** | Round-robin with preempt timer, FPU lazy-restore (#NM), `SpawnTask`/`AddTask` (no `fork`) | Amiga tasks are independent; `fork`/`COW` removed |
| **Signals** | 32-bit bitmask (`sig_recv`/`sig_wait`/`sig_except`), no handlers/frames | Pure `Signal`/`Wait` model — `Wait(mask)` blocks until any bit arrives |
| **IPC** | `MsgPort` + `PutMsg`/`GetMsg`/`WaitPort`/`ReplyMsg`, `Assign` (volumes) | Native Amiga message ports; `kworker` pending for async |
| **Files** | `BPTR` handles, `FileInfoBlock`, `dos_open/read/write/seek/examine/exnext` | DOS layer over VFS (RamFS, FAT32 VFAT/LFN, procfs, devfs, pipe) |
| **GPU/DRM** | `drm_device` + GEM + dma-fence/resv + `drm_sched` + dma-buf/atomic (infra only) | Infra genérica preservada; `gpu/agnus/` (agnus_drv+polaris) compila mas sem probe/init — efetivamente inativo; stub AMDGPU arquivado fora do build |
| **Net** | RTL8139 + ARP/IP/ICMP/UDP (no TCP) + BSD sockets (`bsdsocket.library`) | Amiga-style socket API |
| **Boot** | Multiboot2 → Long Mode, Limine 7.12 (BIOS+UEFI), GRUB fallback | ISO ~3 MB (varia com xorriso/grub-mkrescue, não garantido) |

---

## Syscall Surface (66 traps 0-65, `AOS_*` namespace — fonte canônica: `kernel/include/uapi/aos.h`)

```
AOS_Exit           AOS_SpawnTask      AOS_Read           AOS_Write
AOS_Open           AOS_Close          AOS_Wait           AOS_LoadSeg
AOS_SetBrk         AOS_AllocMem       AOS_FreeMem        AOS_DoIO
AOS_FindTask       AOS_Yield          AOS_Delay          AOS_GetSysTime
AOS_AddTask        AOS_Signal         AOS_SetSignal      AOS_ReturnSignal
AOS_SendSignal     AOS_Pipe           AOS_Seek           AOS_Examine
AOS_Clock          AOS_PutStr         AOS_IoErr          AOS_SetIoErr
AOS_SetProcGroup   AOS_GetProcGroup   AOS_SetConProc     AOS_GetConProc
AOS_CreateDir      AOS_DeleteDir      AOS_DeleteFile     AOS_CurrentDir
AOS_CurrentDirFD   AOS_LockCWD        AOS_Rename         AOS_ExamineDir
AOS_Socket         AOS_Bind           AOS_Send           AOS_Recv
AOS_CloseSocket    AOS_Flush          AOS_Assign
AOS_CreatePort     AOS_DeletePort     AOS_PutMsg         AOS_GetMsg
AOS_WaitPort       AOS_ReplyMsg
AOS_Select         AOS_SetSockOpt     AOS_GetSockOpt     AOS_GetSocketAddr
AOS_SocketIOCtl    AOS_SocketBaseTags AOS_SendTo         AOS_RecvFrom
AOS_CreatePool     AOS_DeletePool     AOS_AllocPooled    AOS_FreePooled
AOS_PoolAvail
```

> `AOS_SetProcGroup/GetProcGroup/SetConProc/GetConProc` (28-31) estão publicados no header mas ainda sem handler — caem em `NOT_FOUND` (`kernel/syscall.c`). Mapa completo em `uapi/aos.h:20-89`, `AOS_NR_SYSCALLS=66`.

Aliases `SYS_*` seguem existindo só para compat de fonte (`SYS_EXIT`, `SYS_FORK`, `SYS_READ`, etc. em `uapi/aos.h:93-106`, consumidos via `AOS_*` no runtime). Flags `AOS_O_*` vivem só no header público. `errno` legado (`ENOENT`, `EINVAL`, ...) mapeia para `AOS_ERR_*` em `kernel/include/syscall.h`. Sem `fork`/`COW`, sem `fd_array`/`pgid` — mas com `pid` por tarefa.

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
│   │   │   ├── agnus/      # agnus_drv+polaris (compila/linkado, sem probe — inativo)
│   │   │   └── descontinued/amdgpu_stub/ # caminho real: kernel/drivers/gpu/descontinued/amdgpu_stub/ — protótipo arquivado, fora do build
│   │   ├── net/             # rtl8139
│   │   └── ...              # framebuffer/screen, keyboard (ABNT2), mouse, serial, timer (PIT 100Hz), RTC, ATA, TTY/terminal, WM, IOMMU
│   ├── exec/                # ExecBase (SysBase), library system, MEMF alloc, Memory Pools (bump allocator)
│   ├── fs/                  # VFS, RamFS, FAT32, procfs, devfs, pipe, bcache, firmware cpio, assign
│   ├── ipc/                 # MsgPort (128B messages)
│   ├── kernel/              # DMA ops, workqueue, panic (timer PIT fica em drivers/timer.c)
│   ├── loader/              # ELF loader
│   ├── mem/                 # PMM bitmap, VMM 4-level, kheap, uaccess
│   ├── net/                 # ARP, IP, ICMP, UDP, loopback, core, bsdsocket
│   ├── pci/                 # PCI scan func0, MSI/MSI-X, BAR WC, IOMMU VT-d identity
│   ├── dos/                 # DOS layer (dos.c, path.c)
│   ├── dogin/               # shell dogin (dogin.c)
│   ├── acpi/                # ACPI RSDP/XSDT/DMAR
│   ├── lib/                 # string helpers
│   ├── include/             # headers internos + uapi/ + compat/ + drm/ + exec/ + dos/
│   ├── kernel.c             # Init sequence → idle
│   ├── syscall.c            # dispatcher das 66 traps AOS_* (28-31 sem handler → NOT_FOUND)
│   └── task.c               # Scheduler, signal bitmask, task mgmt
├── scripts/                 # só create_disk.sh (mcopy/mkfs.vfat) — auxiliar de disco
├── vgpu/                    # QEMU fork + hw/display models + vgpu/scripts/run-test.sh (CI de GPU)
├── limine/                  # Vendored Limine 7.12 (BOOTX64.EFI, limine-bios.sys, etc.)
├── limine.cfg               # Limine PROTOCOL=multiboot2
├── grub.cfg                 # GRUB fallback (multiboot2)
├── linker.ld                # 1M base, PHDRS text R E / data RW
└── Makefile                 # Limine xorriso+bios-install (default), GRUB fallback + abi-check + fat32-check
```

---

## Building

```bash
# Requirements (build ativo)
nasm gcc ld xorriso grub-file grub-mkrescue qemu-system-x86_64 qemu-img limine(7.12 vendored em limine/)
# Só para scripts/create_disk.sh: mformat mcopy mkfs.vfat
# Só para teste UEFI manual: OVMF

make              # Limine BIOS+UEFI ISO (default, ~3 MB — tamanho varia, não garantido)
make grub-iso     # GRUB fallback (ISO maior, varia com grub-mkrescue)
make run          # QEMU SDL (BIOS)
make run-vnc      # QEMU VNC :0
make abi-check    # trava ABI pública (tests/uapi_abi.c — subset: números-chave, aliases, flags, layouts DRM)
make fat32-check  # regressão FAT32 host-side (tests/fat32_safety.c)
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

Em todo boot: `drm_gem_test` + `dma_test_run_all` + `assign_test`/`msgport_test` + testes de `compat`, `dma_buf`, `drm_sched` e `drm_atomic`. O stub AMDGPU arquivado não é compilado nem executado.

---

## Current Status (v0.3 — existência ≠ produção; ver docs/PRODUCTION_READINESS.md, hoje M1)

| Subsystem | Status |
|-----------|--------|
| Memory (PMM/VMM/kheap) | ⚠️ VMA + W^X hardening + heap expansível; sem ASLR/slab, sem testes de estresse |
| Scheduler (round-robin + timer, FPU lazy) | ⚠️ Funcional, preempção/cooperatividade em revisão |
| Signal bitmask (32-bit) | ⚠️ Estrutura ok, falta atomicidade/wakeup formal |
| **Wait blocking real (timeout)** | ⚠️ Funcional (TASK_WAITING + timer), com timer no stack — revisar lifetime |
| MsgPort / Assign | ⚠️ FIFO + selftest ok, sem owner/quota/teardown seguro |
| DOS layer (BPTR, FileInfoBlock) | ⚠️ Fiado aos syscalls, sem fd-table por processo |
| VFS + RamFS + FAT32 VFAT/LFN | ⚠️ RW + validações de mount, sem page cache / ainda sem corpus fsck |
| procfs / devfs / pipe / bcache | ⚠️ Existem; pipe >4 KiB trava; bcache faz I/O sob spinlock |
| PCI + MSI/MSI-X + IOMMU VT-d | ⚠️ Enum func0 + MSI/MSI-X básico; IOMMU em identity (sem isolamento) |
| DRM/GEM/dma-fence/resv/drm_sched/dma-buf/atomic | ⚠️ Infra existe + selftests; sem isolamento/KMS real |
| Driver AMDGPU | ⏸️ Stub arquivado em `kernel/drivers/gpu/descontinued/amdgpu_stub`; fora do build |
| Driver `gpu/agnus` (agnus_drv+polaris) | ⏸️ Compila/linkado, sem probe/init — inativo; Polaris = bring-up local |
| RTL8139 + IPv4/UDP + bsdsocket | ⚠️ Sem TCP; UDP sem validação completa/checksum/limites |
| TTY (canon, termios, ABNT2) | ⚠️ Existe |
| **Memory Pools (MEMF_CHIP/FAST/PUBLIC)** | ⚠️ Bump allocator; `FreePooled` no-op; `MEMF_*` com mapeamento placeholder |
| dogin shell | ⚠️ `Set/Unset` env + `If/While/Repeat/Execute` já existem; `Run`/pipes/redirect pendentes |

---

## Roadmap (Next — ordem do docs/PRODUCTION_READINESS.md: fronteira uaccess → lifecycle → VMM → testes QEMU)

1. **IORequest async** (`SendIO`/`WaitIO`/`AbortIO` via MsgPort + kworker)
2. **dogin** — pipes, redirect, `Run` (env/`Execute`/`If`/`While` já existem)
3. **Intuition** — Layers (damage-rectangle), Screens, Windows, Gadgets (só após P0/P1)
4. **Datatypes** — ELF/PNG/IFF/text loaders
5. **GPU** — porte real só sobre compat layer definida; `gpu/agnus` fica como bring-up Polaris até lá

---

## Related

- **vgpu** — GPU virtualization fork: https://github.com/asimplestray/vgpu

---

## License

MIT — see [LICENSE](LICENSE)