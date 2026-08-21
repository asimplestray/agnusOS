# ApolloOS Kernel — Roadmap Crítico

## Estado Atual (v0.2-Alpha)
- Boot multiboot2 (GRUB) → Long Mode ✅
- IDT/PIC/GDT/TSS (IST p/ double fault) ✅
- PMM (bitmap) + VMM (4-level paging) ✅
- Kernel heap (kmalloc/kfree) ✅
- **#PF handler + demand paging + COW + stack growth** ✅ (`vmm_page_fault_handler`)
- Task scheduler **preemptivo** + FPU/SSE save/restore (fxsave/fxrstor) ✅
- **Signals** (sigaction/sigprocmask/sigreturn/kill, SIGSEGV/SIGKILL, signal frame no user stack) ✅
- **46 syscalls**: fork (COW), execve (ELF64), waitpid, brk, mmap/munmap, pipe, dir ops, job control, sockets ✅
- VFS + RamFS + **FAT32 read-write** (VFAT LFN, mkdir/rmdir/unlink/rename/getdents) + write-back block cache + procfs + devfs + pipe ✅
- **Firmware loader** (`request_firmware`: initrd cpio → ramfs `/lib/firmware` → built-in fallback) ✅
- **Workqueue + timerwheel** (`work_struct`, `queue_work`, `queue_delayed_work`, `cancel_*_sync`, `flush_workqueue`) ✅
- **DMA API** (`dma_dev`: alloc_coherent, map_page/map_sg, sync_*, set_mask — ops table) ✅
- PCI enum + **MSI enable/disable + MSI-X vector masking** + `request_irq()` com **IRQ threading** ✅
- RTC driver, TTY line discipline + job control, panic/backtrace, log ring buffer ✅
- Network stack: RTL8139 + ARP/IP/ICMP/UDP + socket syscalls + loopback (127.0.0.1) ✅
- DRM core nativo: `drm_device_t/file/minor`, ioctl dispatch, `/dev/dri/card0` via devfs ✅
- **GEM** (BO create/destroy/mmap/refcount, VRAM carveout, shrinker stub, selftest no boot) ✅
- **dma-fence + dma-resv** (fence/callback/timeline, reservation object 1 writer/N readers, testes no boot) ✅
- Driver Polaris/R500: probe PCI, modeset 1920×1080×32, test pattern ✅
- **apollo_drv.c**: driver de validação que exercita DRM core + GEM + fence + ring + IRQ de ponta a ponta (base pra compat layer do amdgpu) ✅

---

## Gaps Restantes

| # | Item | Status | Por que importa |
|---|------|--------|-----------------|
| **1** | **dma-buf / PRIME** (`dma_buf.c`) | 🔲 Zero | Compartilhar BO entre drivers; PRIME |
| **2** | **DRM Scheduler** (`drm_sched.c`) | 🔲 Zero | Job queue/entity/runqueue/timeout p/ amdgpu |
| **3** | **Atomic KMS** (`drm_atomic.c`) | 🔲 Zero | Commit atômico plane/crtc/encoder/connector |
| **4** | **Compat layer** (`kernel/include/compat/linux_*.h`) | 🔲 Zero | Port do amdgpu em si |
| **5** | **IOMMU real** (DMAR via ACPI) | ⚠️ Só DMA API default + stub | GTT page tables, BO em RAM do sistema |
| **6** | **MSI-X allocation completa** | ⚠️ Só mask vector | Múltiplos vetores por device (vblank/EOP/SDMA) |
| **7** | **FPU lazy/XSAVE otimizado** | ⚠️ fxsave/fxrstor full save | Performance do context switch |

---

## Próximos Passos (ordem)

### ✅ 1. Page Fault Handler (#PF) — **CONCLUÍDO**
- [x] ISR #PF (vector 14) com error code
- [x] `vmm_page_fault_handler(addr, error_code, rip)`
- [x] Demand paging: página zero-filled on fault
- [x] COW: bit custom no PTE (`VMM_FLAG_COW`), clone on write, `vmm_mark_cow_user_pages()`
- [x] Stack growth: expansão down on guard page fault

### ✅ 2. Preemptive Scheduler + FPU/SSE — **CONCLUÍDO**
- [x] Timer IRQ → `schedule()` preemptivo
- [x] `fxsave_area[512]` em `task_struct`, fpu_save/fpu_restore no switch
- [x] TSS com IST para double fault

### ✅ 3. Signals Básicos — **CONCLUÍDO**
- [x] `sigaction_t`, `sigpending`, `NSIG`
- [x] `sys_rt_sigaction`, `sys_rt_sigprocmask`, `sys_rt_sigreturn`, `sys_kill`
- [x] `do_signal()` no retorno de syscall/interrupt + signal frame no user stack

### ✅ 4. PCI MSI (+ MSI-X parcial) — **CONCLUÍDO**
- [x] `pci_enable_msi()` / `pci_disable_msi()` (32/64-bit)
- [x] `pci_msix_mask_vector()`
- [ ] Allocation dinâmica de múltiplos vetores MSI-X (restante)

### ✅ 5. Firmware Loader — **CONCLUÍDO**
- [x] `request_firmware(fw, name, device)`
- [x] Parse de initrd cpio via tag multiboot2 module
- [x] Cache: initrd → ramfs `/lib/firmware` → tabela built-in

### ✅ 6. Workqueue / Timerwheel — **CONCLUÍDO**
- [x] `work_struct`, `workqueue_struct`, `queue_work()`, `queue_delayed_work()`
- [x] Timerwheel (buckets) para delayed work, processado no timer tick

### ⚠️ 7. IOMMU / DMA API — **PARCIAL**
- [x] `kernel/kernel/dma.c`: ops table (`dma_alloc_coherent`, `map_sg`, `sync_*`, `set_dma_mask`)
- [ ] IOMMU page tables reais (DMAR parsing via ACPI)

---

## Infra DRM (port do amdgpu) — ver GPU_PORTING_STRATEGY.md

| Componente | Arquivo | Status |
|------------|---------|--------|
| DRM Core | `kernel/drivers/drm/drm_device.c` | ✅ ioctl dispatch + `/dev/dri/card0` |
| GEM | `kernel/drivers/drm/drm_gem.c` | ✅ BO + VRAM carveout + selftest |
| dma-fence | `kernel/drivers/drm/dma_fence.c` | ✅ timeline + callbacks |
| dma-resv | `kernel/drivers/drm/dma_resv.c` | ✅ 1 writer / N readers |
| Testes | `kernel/drivers/drm/dma_test.c` | ✅ roda no boot (`dma_test_run_all`) |
| dma-buf | `kernel/drivers/drm/dma_buf.c` | 🔲 Fase 2 |
| drm_sched | `kernel/drivers/drm/drm_sched.c` | 🔲 Fase 2 |
| Atomic KMS | `kernel/drivers/drm/drm_atomic.c` | 🔲 Fase 2 |
| Compat layer | `kernel/include/compat/linux_*.h` | 🔲 Fase 2 |

---

## Driver Polaris/amdgpu (DEPOIS da Fase 2 do port)

| Fase | Componente | Depende de |
|------|------------|------------|
| 1 | PCI + BARs + MSI + VRAM/GTT + BO allocator | ✅ base pronta |
| 2 | GFX ring + CP packets + IB submit + fence | ✅ base pronta (validado via apollo_drv) |
| 3 | Interrupts (vblank, EOP, SDMA) + drm_sched | drm_sched (Fase 2), MSI-X completo |
| 4 | DCN 1.0 display + atomic KMS | atomic KMS (Fase 2) |
| 5 | SMU firmware + DPM + PP_TABLE parser | firmware loader ✅, workqueue ✅ |
| 6 | VCN video + audio | firmware loader ✅ |

---

## Como testar

```bash
# Build + boot padrão (QEMU SDL)
make run

# Sem GUI (VNC em localhost:5900)
make run-vnc

# Boot com vGPU emulada (fork do QEMU em vgpu/)
vgpu/scripts/run-test.sh apolloos.iso 2G nvidia-gt730
# devices disponíveis: nvidia-gtx750ti/gtx1080/rtx2080, amd-rx480/rx6700xt, intel-arc-a770
```

No boot o kernel roda automaticamente:
- `drm_gem_test()` — selftest do GEM (BO create/write/read/unmap)
- `dma_test_run_all()` — testes de fence, resv e timelines concorrentes
- Probe Polaris (device IDs `0x67DF/0x67EF/0x67FF`) → modeset 1920×1080×32 + test pattern

---

## Notas de Implementação

### #PF Error Code bits (x86_64)
```
Bit 0: P    = 0 → page not present, 1 → protection violation
Bit 1: W/R  = 0 → read, 1 → write
Bit 2: U/S  = 0 → supervisor, 1 → user
Bit 3: RSVD = 1 → reserved bit set in PTE
Bit 4: I/D  = 1 → instruction fetch
```

### COW Implementation (implementada)
- PTE bit custom (`VMM_FLAG_COW`) = COW flag
- On write fault: aloca página nova, copia, atualiza PTE (limpa COW, seta WRITE)
- `vmm_mark_cow_user_pages(pml4_phys)` marca páginas user writable durante o fork

### FPU/SSE Save/Restore (implementada)
```asm
fxsave [task->fxsave_area]   ; 512 bytes, aligned 16
fxrstor [task->fxsave_area]
```

### apollo_drv.c — papel do driver
O `apollo_drv.c` **não é um driver de produção**: é um driver de validação que se registra
no DRM core como um driver completo (load/unload/open/release, GEM create/free,
submit_command, wait/signal fence, mode_set, IRQ handler) para exercitar todas as APIs
nativas (memória, ring, fence, workqueue, IRQ) antes de portar o amdgpu de verdade.
Ele define o formato da futura compat layer.

---

## Status Tracker

- [x] #PF handler + demand paging + COW + stack growth
- [x] Preemptive scheduler + FPU/SSE
- [x] Signals (SIGSEGV, SIGKILL, sigreturn)
- [x] PCI MSI (+ MSI-X masking)
- [x] Firmware loader
- [x] Workqueue / timerwheel
- [x] DMA API (ops table) — falta IOMMU real
- [x] DRM Core + GEM + dma-fence + dma-resv (Fase 1 do port)
- [ ] dma-buf / drm_sched / atomic KMS / compat layer (Fase 2 do port)
- [ ] Driver Polaris/amdgpu Fases 3-6

---

*Última atualização: 2026-08-21*
