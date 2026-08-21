# ApolloOS GPU Porting — Divisão de Tarefas (4 Devs × 5 Fases)

> Derivado de `GPU_PORTING_STRATEGY.md`. Cada fase tem 1 tarefa por dev.
> Critério de entrega padrão de toda tarefa: código compilado no `make`, boot em QEMU sem regressão e sem debug temporário.

| Fase | Tema | Prazo | Status |
|------|------|-------|--------|
| 1 | Infra DRM Core + Memória | Mês 1 | ✅ **Concluída** |
| 2 | Scheduler, Atomic KMS base, Compat Layer | Mês 1–2 | 🔲 Próxima |
| 3 | amdgpu v0.1.0 MINIMAL (sem DC) | Mês 2–3 | 🔲 |
| 4 | Display Core (DC) por geração | Mês 3–5 | 🔲 |
| 5 | DPM/PowerPlay + Compute + Validação | Mês 5–6 | 🔲 |

---

## Fase 1 — Infra DRM Core + Memória ✅ CONCLUÍDA

### Dev 1 — DRM Core nativo ✅
- Criar `kernel/drivers/drm/drm_device.c` + `drm_driver.h`: `drm_device_t`, `drm_file_t`, `drm_minor_t`, `drm_ioctl` dispatch com tabela de ioctls.
- Substituir stubs atuais em `drm_driver.h/.c` pela versão real.
- **Entrega:** `drm_init()` roda no boot; registro de device com 1 minor (`/dev/dri/card0`). → **Entregue** (commit `5b276ab`).

### Dev 2 — GEM (Graphics Execution Manager) ✅
- Criar `kernel/drivers/drm/drm_gem.c`: BO create/destroy/mmap/refcount, VRAM carveout, shrinker stub.
- APIs: `gem_create`, `gem_mmap_phys`, refcount com spinlock.
- **Entrega:** userspace kernel test cria BO, escreve, lê, unmapa (via dev3_test-style probe). → **Entregue** (`drm_gem_test()` roda no boot; commit `d4bf848`).

### Dev 3 — dma-fence + dma-resv ✅
- Criar `kernel/drivers/drm/dma_fence.c` e `dma_resv.c`: fence + callback + timeline, reservation object (1 writer / N readers).
- **Entrega:** teste de 2 timelines concorrentes com callback disparando em ordem. → **Entregue** (`dma_test.c` com fence/resv/timeline tests via `dma_test_run_all()` no boot; commit `8c386ae`).

### Dev 4 — Firmware Loader + PCIe exts ✅
- Criar `kernel/fs/firmware.c`: `request_firmware()`, fallback built-in (tabela estática), caminho `/lib/firmware` (ramfs), parse de initrd cpio via tag multiboot2 module.
- Estender PCI: MSI enable/disable, MSI-X mask vector, mapeamento de BAR (write-combine via VMM), IOMMU stub + DMA API ops table em `kernel/kernel/dma.c`.
- **Entrega:** firmware busca built-in → ramfs → erro; BAR da GPU mapeado e legível. → **Entregue** (commit `188277f`).

> **Bônus entregue fora da fase:** `apollo_drv.c` — driver de validação registrado no
> DRM core que exercita load/unload, GEM, submit_command, fences, mode_set e IRQ handler
> de ponta a ponta, validando as APIs da Fase 1 antes do port do amdgpu.

---

## Fase 2 — Scheduler, Atomic KMS base, Compat Layer 🔲 PRÓXIMA

> Pré-requisitos já disponíveis da Fase 1: DRM core, GEM, dma-fence/dma-resv,
> firmware loader, workqueue/timerwheel, DMA API.

### Dev 1 — dma-buf / PRIME
- Criar `kernel/drivers/drm/dma_buf.c`: export/import, mmap, sync_file.
- **Entrega:** BO exportada por um driver e importada por outro (2 devices fake) com mmap compartilhado.

### Dev 2 — DRM Scheduler
- Criar `kernel/drivers/drm/drm_sched.c`: job queue, entity, runqueue com workqueue existente, timeout/recovery.
- **Entrega:** fila de 1000 jobs completa em ordem; job que faz timeout é abortado + callback de recovery.

### Dev 3 — Atomic KMS base
- Criar `kernel/drivers/drm/drm_atomic.c`: plane/crtc/encoder/connector, propriedades, commit com estado validado.
- **Entrega:** commit atômico de CRTC+connector com rollback em caso de validação falhar (teste unitário no kernel).

### Dev 4 — Compat Layer headers
- Criar `kernel/include/compat/linux_*.h` (list, mutex, work, fence, dma_buf, module) — wrappers `static inline` finos, 1:1 com nativos.
- Não vazar `kmalloc/list.h` pro kernel: revisar include e proibir no review.
- **Entrega:** `drm/core` compila 100% usando headers compat; documentar convenção no header.

---

## Fase 3 — amdgpu v0.1.0 MINIMAL (sem DC)

### Dev 1 — HW Init + ASIC detect
- Portar `amdgpu_device_init()` minimal: detect de ASIC por PCI ID (Polaris/VI primeiro), alocar `adev`, init de regs base, IRQ.
- Desativar compute/SDMA rings (default: só GFX).
- **Entrega:** boot log mostra `amdgpu: Polaris20 detected`, MMIO register dump coerente com specs.

### Dev 2 — VRAM/GTT Managers
- Portar `amdgpu_vram_mgr` + `amdgpu_gtt_mgr` sobre GEM nativo da Fase 1.
- **Entrega:** alocar BO em VRAM (carveout) e em GTT (RAM), ler-back com padrão conhecido OK.

### Dev 3 — Modeset básico (fbdev linear, sem DC)
- CRTC/encoder/connector simples via regs DCE: scanout de framebuffer linear a partir de BO VRAM, modo fixo (ex: 1920×1080@60 ou resolução de teste).
- **Entrega:** console gráfico do ApolloOS aparece na tela via GPU (antes era VGA std).

### Dev 4 — GFX ring + thermal monitor
- Portar GFX ring submit (regs CP) + `gpu_test_pattern.c` (HSV animado, F12 liga/desliga) e `thermal_monitor.c` (poll 2s, warn 85°C, crit 95°C, PANIC).
- Forçar `amdgpu_dpm_force_performance_level(AMD_DPM_FORCED_LEVEL_LOW)` desde o init.
- **Entrega:** test pattern roda no ring GFX 24/7 sem hang; log térmico no serial.

---

## Fase 4 — Display Core (DC) por geração

### Dev 1 — DCE 8/10/11 (GCN 1–3, VI)
- Portar estrutura core do DC (dal core, resource, stream encoder) para DCE 8/10/11: Polaris, HD 7xxx, R9 2xx.
- **Entrega:** modeset via DC (não fbdev) em ASIC GCN com connector/encoder reais.

### Dev 2 — DCN 1/2 (Vega, Navi 1x/2x)
- Portar DCN 1/2: disby, optc, hubp, mmhubbub, splitter.
- **Entrega:** modeset via DC em Vega/Navi baseado nos mesmos IRQ/policy da DCE.

### Dev 3 — DC link/transmitter + MST
- Portar link encoder/transmitter parametrizado por ASIC e MST (topologia AUX).
- **Entrega:** hotplug detectado via DP MST; 2 monitores por hub MST.

### Dev 4 — Atomic commit DC + substituição do fbdev
- Integrar atomic KMS (Fase 2) com DC: properties (mode, scale, underscan), commit plane/crtc.
- Remover fbdev linear como padrão; manter como fallback.
- **Entrega:** `modetest` interno troca resolução/refresh sem tearing; fbdev só em crash path.

---

## Fase 5 — DPM/PowerPlay + Compute + Validação

### Dev 1 — PowerPlay / DPM
- Portar DPM framework + tabelas Polaris (clocks/voltages), SMPU, fan control.
- **Entrega:** GPU em idle baixa clocks (ex: 300MHz), fan curva funcional, throttle sob carga.

### Dev 2 — Compute + SDMA rings + firmware por ASIC
- Habilitar compute/SDMA rings com firmware blobs por geração (built-in, licença AMD) e VCN (decode) se seguro.
- **Entrega:** compute job simples (add kernel) roda; SDMA copy BO→BO OK.

### Dev 3 — Testes / CI / Thermal hardening
- `make iso` + script CI boot-and-test em QEMU (sem GPU: só regressão kernel); teste real em Polaris/Vega/Navi com checklist (`lspci -vv`, dmesg, repro steps).
- Subir níveis: warn/crit com fan override e shutdown gracecioso.
- **Entrega:** CI verde + checklist documentado + N boots de 1h sem hang.

### Dev 4 — Milestones/GitHub + i915 garante DRM core
- Criar milestones e issues por tarefa (template com GPU model/dmesg/repro), docs por geração (quirks, register deltas), rotação de devs.
- Validar que DRM core (Fase 1–2) serve i915 sem mudanças de API; corrigir vazamentos genéricos.
- **Entrega:** issues rastreáveis, docs por ASIC, DRM core 100% genérico.

---

## Ordem de execução & dependências

- **Fase 3 trava na Fase 1+2** (GEM/fence/sched/compat prontos). Fase 2 pode começar em paralelo com o fim da Fase 1.
- ✅ **Fase 1 concluída** — DRM core, GEM, dma-fence/dma-resv, firmware loader e DMA API já no `master`, com selftests rodando no boot (`drm_gem_test()`, `dma_test_run_all()`).
- Dev 3 (KMS base) e Dev 1 (drm core) precisam alinhar a API de commit antes da Fase 4.
- **Gate de qualidade:** nada entra em `compat/` ou `drm/` sem code review do tech lead (regra do doc, seção 7).