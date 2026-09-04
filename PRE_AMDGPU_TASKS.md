# ApolloOS — Tasks para terminar ANTES do porte amdgpu (pré-Fase 5)

> **Objetivo:** fechar buracos do kernel base que quebram o `amdgpu v0.1.0` em HW real `0x1002:6FDF` (RX 590 GME) e bloqueiam `DOOM_GPU_FALLBACK.md`. Fase 1-4 já estão `✅` em `vgpu` `amd-rx480` (`GPU_PORTING_TASKS.md:12`), mas `vgpu` não modela IOMMU/MSI-X/EFI real.
> **Critério de aceite global:** `make clean && make` sem `WARN RWE`, `grub-file --is-x86-multiboot2 build/iso/boot/apolloos.bin` OK, boot QEMU BIOS `qemu-system-x86_64 -m 512M -cdrom apolloos.iso -display none -serial file:serial.log -vga std` com `serial.log` contendo `PASS` em `drm_gem_test/dma_test/compat_layer_test/dma_buf_test/drm_sched_test/drm_atomic_test` + `amdgpu_init` gracioso sem `PANIC`. Cada task termina com `selftest` no boot ou `QEMU OVMF` sem `X64 Exception #PF`.

---

## P0 — Blockers de boot (sem isso não sobe em HW real)

### P0.1 Fix híbrido BIOS+EFI + ELF PHDRs
- **Problema:** `Makefile:66` com `-d /usr/lib/grub/x86_64-efi` gera ISO EFI-only sem `boot_hybrid.img` (QEMU BIOS sem output). `linker.ld:3` gerava 1 `LOAD RWE` (`0xffffffff` `readelf -l build/iso/boot/apolloos.bin`) que EFI marca `W^X`.
- **Arquivos:** `Makefile:66`, `linker.ld:4-33`, `boot/boot.asm:1`, `grub.cfg:1`
- **Fazer:** `grub-mkrescue -o $(ISO_OUT) build/iso` (sem `-d`, híbrido `i386-pc`+`x86_64-efi`), `linker.ld` `PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }` e `.text/.rodata/.eh_frame :text` vs `.data/.bss :data`
- **Aceite:** `readelf -l` mostra `LOAD R E` + `LOAD RW` (não `RWE`), `xorriso` log `Copying to System Area: boot_hybrid.img`, boot BIOS `serial.log` `ApolloOS: Starting kernel...` e boot EFI `OVMF_CODE.4m.fd` sem `X64 Exception` antes do kernel
- **Estado atual:** parcial (BIOS OK em `18:40`, EFI ainda `X64 Exception Type - 0E` `RIP 1B76xxxx CR2 1B76xxxx Error 0003` mesmo após PHDR fix — testado `minimal/efi_gop/no-fb` todos PF)

### P0.2 GRUB EFI multiboot2 PF em OVMF → FIXED via Limine 7.12.0
- **Problema:** com `OVMF_CODE.4m.fd` `BdsDxe: Booting 'ApolloOS'` → `!!!! X64 Exception #PF ... ImageBase 0xF3D008` antes de `kernel_main` no GRUB EFI `multiboot2.mod` (testado `minimal/efi_gop/no-fb` todos PF, `WIP boot.asm CS.base` não resolveu). Afeta `DOOM_GPU_FALLBACK.md:88` fallback `UEFI GOP Linear FB`.
- **Solução:** portado para **Limine 7.12.0** `PROTOCOL=multiboot2` `limine.cfg:4` (`KERNEL_PATH=boot:///boot/apolloos.bin` `MODULE_PATH=boot:///boot/fw.cpio`) — Limine `BOOTX64.EFI` `limine-uefi-cd.bin` `limine-bios-cd.bin` `limine-bios.sys` em `limine/` + `Makefile:56` `xorriso -as mkisofs -b boot/limine-bios-cd.bin ... --efi-boot boot/limine-uefi-cd.bin` + `limine/limine bios-install`. Mantido `grub-iso` fallback `make grub-iso`.
- **Arquivos:** `boot/boot.asm:9-21`, `limine.cfg:1`, `limine/BOOTX64.EFI`, `kernel/kernel.c:40` `fb_init(multiboot_info)` , `kernel/drivers/framebuffer.c:1`, `Makefile:56`
- **Aceite:** `qemu-system-x86_64 -m 512M -cdrom apolloos.iso` BIOS `SeaBIOS` **PASS** `176` linhas `serial.log` + `qemu -bios OVMF_CODE.4m.fd` UEFI **PASS** `ApolloOS: Starting kernel` `FW: cache initialized` `ASSIGN/MSGPORT PASS` (`172` linhas, `DRM-SCHED` FIFO `jobs completaram fora de ordem` em UEFI é bug conhecido `timer 100Hz` + `workqueue` passivo `P1.3`, não bloqueia boot)
- **Dependência:** P0.1

### P0.3 Mapeamento PCI BARs real Polaris `BAR0/BAR1/BAR5` sem `sizing` que quebra KVM — ✅ IMPLEMENTADO `ec2dbb8`
- **Problema:** `kernel/pci/pci.c:218` `pci_get_bar_size` escreve `0xFFFFFFFF` no BAR — `GPU_PORTING_TASKS.md:175` que quebra `KVM` slot (`vgpu` fix removeu sizing). Em `kernel/drivers/gpu/amd/amdgpu/amdgpu_device.c:106` já usa `CONFIG_MEMSIZE` mas `pci_map_bar_wc:230` ainda chamava `pci_get_bar_size` → size `0`→ fallback `4096` incorreto para VRAM 8GB `DOOM_GPU_FALLBACK.md:95`
- **Solução:** `kernel/pci/pci.c:230` `pci_map_bar_wc` agora detecta `vendor 0x1002` e usa tamanho fixo por `dev_id/bar` igual a `amdgpu_device.c:88-106` (`0x6FDF BAR0 256M/BAR5 256K`, vgpu `BAR0 16M/BAR1 256M`), só faz `pci_get_bar_size` para não-GPU. `vmm_map_region:251` com `WC` intacto.
- **Arquivos:** `kernel/pci/pci.c:230` (fix `ec2dbb8`), `kernel/include/amdgpu.h:50`, `kernel/drivers/gpu/amd/amdgpu/amdgpu_device.c:106`
- **Aceite:** `amdgpu_init` em `vgpu amd-rx480` e em HW real `lspci -s 01:00.0 -vv` size coerente, `amdgpu_mem_selftest() PASS`, `serial.log` sem `WRITE_DATA fora da VRAM descartado` `kernel/drivers/gpu/amd/amdgpu/amdgpu_gfx.c:204` — **PASS** em `BIOS 176` linhas
- **Esforço:** 1 dia → feito

---

## P1 — Blockers de DMA/IRQ (amdgpu não funciona sem)

### P1.1 IOMMU real (DMAR via ACPI) — `KERNEL_TODO.md:39`
- **Problema:** `kernel/kernel/dma.c:16` `default_alloc_coherent` `pmm_alloc_block()` + identity `*dma_handle=phys`, `kernel/pci/pci.c:282` `pci_iommu_map` stub `iommu_map(NULL,identity)`, `kernel/kernel/dma.c:263` `iommu_map` stub. GTT `amdgpu_vram_mgr.c` (GTT=RAM) sem tradução falha com IOMMU enabled em `0x6FDF`.
- **Arquivos:** `kernel/kernel/dma.c:1-299`, `kernel/include/dma.h:1`, `kernel/mem/pmm.c:58` `pmm_init(multiboot_info)` `mmap_tag`, `kernel/mem/vmm.c:15` `kernel_pml4_phys`
- **Fazer:** ACPI parse `DMAR` (via `multiboot_tag` + `rsdp`), `iommu_init()` `dma.c:263` cria `iommu page tables` 4-level, `pci_iommu_map:282` instala `iova→paddr` com `VMM_FLAG_PRESENT|WRITE`, `dma_map_page:32` retorna `iova` não `page` identity. Fallback identity se `DMAR` ausente (QEMU).
- **Aceite:** `serial.log` `IOMMU: DMAR found, 1 IOMMU @ ...` ou `IOMMU: Not present, using identity`, `dma_buf_test()` cross-device `B le padrão escrito por A` ainda PASS em QEMU, `amdgpu_bo_create(...,false) GTT` `amdgpu_mem_selftest` GTT readback OK em HW real com `intel_iommu=on`
- **Depende:** P0.3
- **Esforço:** 1-2 sem

### P1.2 MSI-X allocation dinâmica multi-vetor (vblank/EOP/SDMA) — `KERNEL_TODO.md:40` — ⚠️ PARCIAL `ec2dbb8`
- **Problema:** `kernel/pci/pci.c:110` só `pci_msix_mask_vector`, `pci_enable_msix:151` existia mas codificava `vectors[i]` direto como `addr` (0) e `data` 0, sem APIC MSI `0xFEE00000`.
- **Solução parcial:** `kernel/pci/pci.c:177` `pci_enable_msix` agora detecta `vec<256` como número e codifica `msg_addr 0xFEE00000` + `msg_data 0x4000|vec` (edge, BSP APIC 0), mantém compat se já codificado. Preparado para 3 vetores `vblank/EOP/SDMA` para `amdgpu` via `request_irq` futuro. `request_irq` APIC alloc ainda pendente.
- **Arquivos:** `kernel/pci/pci.c:151` (fix `ec2dbb8`), `kernel/include/pci.h:69`, `kernel/cpu/idt.c:1`, `kernel/drivers/gpu/amd/amdgpu/amdgpu_gfx.c:60`
- **Aceite (parcial):** `pci_enable_msix` com `vec<256` programa `FEE00000` correto; `full` exige `pci_msix_alloc_vectors` + `idt` `APIC 0xFEE00310` + `request_irq` 3 `amdgpu` — pendente
- **Esforço:** 3-5 dias → 1 dia feito, 2-4 dias restantes

### P1.3 Workqueue drenagem automática (kworker) — `KERNEL_TODO.md:43` — ⚠️ PARCIAL `ec2dbb8`
- **Problema:** `kernel/kernel/workqueue.c:79` `queue_work` só enfileira, `flush_workqueue:123` manual, `timerwheel_process:177` faz `queue_work(system_wq)` sem thread. `drm_sched.c:252` `sched_pump_work` depende de `queue_work(sched->wq)` mas `amdgpu_idle_tick()` faz `pattern ~10fps` manual porque `system_wq` passivo.
- **Solução parcial:** `kernel/kernel/workqueue.c:214` adicionado `kworker_main()` `while(1){flush(system_wq/system_long_wq); hlt}` + `workqueue_start_kworker()` chamado após `task_init` `kernel/kernel.c:135`, com `#include <serial.h>` + `workqueue.h:53` proto. **Desabilitado por ora** (`workqueue_start_kworker: log DESABILITADO`) devido a `#GP fxsave 16B` em `task_create:160` `fpu_save` (`kheap` 8B não garante `aligned(16)` para `fxsave_area[512]`), mantém `WQ passivo` mas documenta fix (kheap 16B ou `task_create` sem FPU). `serial.log` agora mostra `kworker: DESABILITADO (P1.3 parcial)`.
- **Arquivos:** `kernel/kernel/workqueue.c:214` (`ec2dbb8`), `kernel/include/workqueue.h:53`, `kernel/kernel.c:135` (call), `kernel/task.c:160` (`fpu_save`)
- **Aceite (parcial):** `WQ` ainda `flush_workqueue` manual, mas infra `kworker` pronta; `full` exige `kheap 16B` + `fpu_used=false` para kthreads + `need_resched` wake. `drm_sched_test()` timeout 3 ticks ainda PASS em BIOS `176` linhas; UEFI `173` linhas com `FIFO fora de ordem` é bug `P1.3` residual
- **Esforço:** 2 dias → 1 dia feito, 1 dia para alinhamento FPU

---

## P2 — Preparação do porte (não bloqueia boot, mas bloqueia DOOM)

### P2.1 SDMA ring (System DMA) para `DOOM_GPU_FALLBACK.md:50-54`
- **Problema:** `kernel/drivers/gpu/amd/amdgpu/amdgpu_gfx.c:125` só `GFX ring 4096dw` `WRITE_DATA/FENCE`, `DOOM` Nível 2 precisa `SDMA_OP_COPY` RAM→VRAM 320x200 ~64KB/frame 35fps, `DOOM_GPU_FALLBACK.md:49` `SDMA Timeout → drm_sched Recovery + CPU memcpy`.
- **Arquivos:** `kernel/drivers/gpu/amd/amdgpu/amdgpu_gfx.c:1-387`, `kernel/include/amdgpu.h:122` `AMDGPU_RING_SIZE_DW`, `kernel/drivers/drm/drm_sched.c:1`
- **Fazer:** `amdgpu_sdma.c` mirror `amdgpu_gfx.c` com `mmSDMA0_RB_RPTR/WPTR`, opcode `SDMA_OP_COPY` + `FENCE`, `drm_sched` `sdma_sched` `timeout 100`, `amdgpu_sdma_selftest()` copy `GTT→VRAM` pattern, integrar `DOOM DG_DrawFrame` `dma_fence` `sync_file`
- **Aceite:** `serial.log` `SDMA ring pronto` `sdma selftest PASS` `WRITE_DATA` em `vgpu` `amd-rx480` (estender `vgpu` `gfx8` com `SDMA regs` se preciso)

### P2.2 Conversor paleta DOOM 8-bit → XRGB8888 + `doomgeneric` import
- **Problema:** `DOOM_GPU_FALLBACK.md:21-27` `DG_Init/DrawFrame/SleepMs/GetTicks/GetKey` ainda não existe; `framebuffer.c` só GOP legacy.
- **Arquivos:** `kernel/demos/doom/` (criar), `kernel/drivers/framebuffer.c:1`, `kernel/drivers/keyboard.c:1`, `kernel/drivers/timer.c:1` `timer_get_ticks`
- **Fazer:** importar `doomgeneric` core, `DG_DrawFrame` → `amdgpu_bo_create` `fb_bo` `kernel/include/amdgpu.h:160` + `SDMA` ou `CPU memcpy` + `dc_flip` `kernel/drivers/gpu/amd/amdgpu/dc/dc_core.c`
- **Aceite:** `DOOM1.WAD` via `module2 /boot/doom1.wad` `grub.cfg:10` exposto `/boot/doom1.wad` `kernel/fs/firmware.c:61`, frame estático em `vgpu` screenshot

### P2.3 Fallback matriz `DOOM_GPU_FALLBACK.md:66` + sonda segura `vgpu` vs HW real
- **Arquivos:** `kernel/drivers/gpu/amd/amdgpu/amdgpu_device.c:106` `polaris.c` removido sizing, `kernel/include/amdgpu.h:105` `mmHPD0_STATUS`
- **Fazer:** implementar `CheckGPU→RenderGPU→CheckGOP→RenderVGA` `DOOM_GPU_FALLBACK.md:66`; `amdgpu_init` falha graciosa já OK `kernel/kernel.c:224` `nenhuma ASIC suportada` → fallback `GOP linear FB` → `VGA 80x25`
- **Aceite:** `qemu -vga std` sem `amd-rx480` mostra `framebuffer GOP`; `qemu -device amd-rx480` mostra `scanout via DC`

---

## P3 — Higiene antes do `git tag linux-6.6` freeze `GPU_PORTING_STRATEGY.md:47`

### P3.1 Definir baseline LTS + firmware blobs
- **Arquivos:** `scripts/make_fw_initrd.sh:8` `FW_LIST` 8 blobs Polaris10
- **Fazer:** travar `drivers/gpu/drm/amd` tag `6.6.30`, documentar `FW_SRC /lib/firmware/amdgpu` + `register_builtin_firmware` fallback, `LICENSE` AMD
- **Aceite:** `build/fw.cpio` 389632 bytes reproduzível `stat -c%s build/fw.cpio`

### P3.2 CI `make iso` + boot-and-test regressão
- **Arquivos:** `Makefile:48`, `vgpu/scripts/run-test.sh:5`, `.farm/` `kernel/kernel.c:128` selftests
- **Fazer:** `scripts/ci_boot.sh` roda `qemu-system-x86_64 -m 512M -cdrom apolloos.iso -display none -serial file:ci.log -vga std` `timeout 15` grep `ALL CHECKS PASSED`
- **Aceite:** CI verde em QEMU sem GPU

### P3.3 Docs vivos + rotação `GPU_PORTING_STRATEGY.md:270`
- **Fazer:** atualizar `KERNEL_TODO.md:188` e `GPU_PORTING_TASKS.md:14` Fase 5 checklist, template `lspci -vv/dmesg/GPU model/repro` `GPU_PORTING_STRATEGY.md:269`
- **Aceite:** `PRE_AMDGPU_TASKS.md` este arquivo linkado em `README.md:88`

---

## Ordem de execução sugerida

1. **P0.1+P0.2** (boot) — 2-3 dias — libera teste em HW real `RX 590 GME` via GOP ou BIOS CSM
2. **P1.3** (kworker) — 2 dias — desbloqueia thermal/IRQ sem hacks
3. **P1.1** (IOMMU) + **P1.2** (MSI-X) — 1-2 sem — libera GTT+IRQs do `amdgpu`
4. **P2.1** (SDMA) → **P2.2/3** (DOOM) — 1 sem + contínuo
5. **P3** (freeze) antes de copiar `drivers/gpu/drm/amd` 6.6

> **Não começar porte `drivers/gpu/drm/amd` 6.6 antes de P0+P1 `✅` — senão `sched timeout` `kernel/drivers/drm/drm_sched.c:166` `ETIMEDOUT 110` e `GTT faults` vão mascarar bugs do port.
