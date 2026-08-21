# ApolloOS GPU Driver Porting Strategy

## Visão Geral

Este documento registra a estratégia acordada para trazer suporte a GPUs AMD modernas (GCN/RDNA) ao ApolloOS via port do driver `amdgpu` do Linux, usando uma camada de compatibilidade fina e port incremental versionado.

## Estado Atual da Fase 1 (Infra DRM Core + Memória)

> Boot QEMU validado (serial): `drm_init` registra `/dev/dri/card0`; selftests de GEM (create/write/read/unmap, carveout, refcount) e DMA fence/resv (2 timelines + callbacks em ordem) passam no boot.

| Subtarefa | Status |
|-----------|--------|
| Dev 1 — DRM Core (`drm_device.c`, `drm_core.c`, `/dev/dri/card0`) | ✅ |
| Dev 2 — GEM (`drm_gem.c`, carveout VRAM, shrinker stub, refcount) | ✅ |
| Dev 3 — dma-fence + dma-resv (`dma_fence.c`, `dma_resv.c`, testes no boot) | ✅ |
| Dev 4 — Firmware loader + PCIe MSI-X/BAR/IOMMU stub (`firmware.c`, `pci.c`) | ⚠️ implementado; falta teste de boot dedicado (critério de entrega) |

**Bloqueios de Fase 2 herdados do kernel base:** resolvidos — `#PF/demand paging + COW` e `scheduler preemptivo + FPU/SSE` já estão concluídos no kernel base (ver `KERNEL_TODO.md`). O scheduler preemptivo (requisito para timeout/recovery do `drm_sched`) e o workqueue/timerwheel já estão disponíveis para a Fase 2.

---

## 1. Estratégia de Port: Version Pinning Incremental

### Princípio Central
> **Portar uma versão LTS estável completamente, congelar, só aplicar bugfixes + device IDs seletivos.**

### Por que NÃO "merge contínuo de 1 mês de commits"
| Fator | Realidade |
|-------|-----------|
| DC (Display Core) | ~200 commits/merge window, APIs mudam |
| PowerPlay/DPM | Tabelas de clocks/voltages mudam por geração |
| Firmware blobs | Novos a cada geração (precisa `.bin` novo) |
| IP blocks novos | VCN, JPEG, etc = código novo inteiro |
| drm_sched/dma_fence | Core DRM muda → quebra compat layer |
| Refactors/cleanups | Conflitos de rebase constantes |

### Roadmap Realista (6 meses)

| Fase | Entrega Nativa | Camada Compat (stub) | Duração |
|------|----------------|---------------------|---------|
| **1-2** | GEM simples (BO linear, VRAM carveout), dma_fence, dma-buf, firmware loader | — | 2 meses |
| **3** | **amdgpu v0.1.0 MINIMAL**: HW init, VRAM/GTT, mode set básico (connector/encoder/crtc simples), **sem DC** (usa fbdev linear) | DC, DPM, powerplay, audio | 1 mês |
| **4-5** | Port DC geração a geração (DCE 8/10/11 → DCN 1/2/3) | DPM, VRR, MST | 2 meses |
| **6+** | Powerplay, compute, 3D (radv) | — | Contínuo |

### Baseline Sugerida
- **Linux 6.6 LTS** (ou 6.1 LTS) — amdgpu estável, documentado, firmware disponível
- Congelar em `drivers/gpu/drm/amd/` dessa versão
- Aplicar apenas: bugfixes de hang/crash, novos PCI IDs, security fixes

---

## 2. Infraestrutura DRM Nativa Necessária (Pré-requisitos)

Antes de tocar no código do amdgpu, o kernel precisa ter estes subsistemas **nativos** (sem compat layer):

| Subsistema | Arquivos | Status Atual | Esforço (1 dev) |
|------------|----------|--------------|-----------------|
| **DRM Core** | `drm_device.c`, `drm_core.c`, `drm_device.h`, `drm_driver.h` | ✅ Implementado — `drm_init()` roda no boot, registra `/dev/dri/card0` (Fase 1 Dev 1) | ~2 sem |
| **GEM (Graphics Execution Manager)** | `drm_gem.c`: BO create/destroy/mmap/refcount, VRAM carveout, shrinker stub | ✅ Implementado — selftest de boot (create/write/read/unmap) passando (Fase 1 Dev 2) | ~2 sem |
| **dma-fence / dma-resv** | `dma_fence.c`: fence + callback + timeline; `dma_resv.c`: reservation object | ✅ Implementado — testes de boot com 2 timelines concorrentes passando (Fase 1 Dev 3) | 1-2 sem |
| **dma-buf / PRIME** | `dma_buf.c`: export/import, mmap, sync_file | ✅ Implementado — attachments, vmap compartilhado, sync_file; selftest 2 devices fake no boot (Fase 2 Dev 1) | ~1 sem |
| **Atomic KMS** | `drm_atomic.c`: plane/crtc/encoder/connector, properties, commit | ✅ Implementado — snapshot old/new, commit all-or-nothing com rollback; selftest no boot (Fase 2 Dev 3) | ~2 sem |
| **DRM Scheduler** | `drm_sched.c`: job queue, entity, runqueue, timeout/recovery | ✅ Implementado — entity FIFO + RR sobre workqueue nativo; selftest 1000 jobs + timeout no boot (Fase 2 Dev 2) | ~1 mês |
| **Firmware Loader** | `firmware.c`: `request_firmware()`, fallback built-in, `/lib/firmware` | ✅ Implementado — `request_firmware()` + `register_builtin_firmware()` + cache do initrd; em validação (Fase 1 Dev 4) | ~2 sem |
| **PCIe/MSI-X + IOMMU stub** | `pci.c` extensions: MSI-X, BAR mapping, IOMMU dummy | ✅ Estendido — MSI/MSI-X enable/mask, BAR write-combine, IOMMU stub; em validação (Fase 1 Dev 4) | 2-4 sem |
| **Compat Layer** | `kernel/include/compat/linux_*.h` | ✅ Implementado — types/list/mutex/spinlock/work/fence/dma_buf/module, convenção documentada; validação runtime no boot (Fase 2 Dev 4) | contínuo |

> **Nota:** Esta infra serve **também para i915** quando for a vez. Escrevam uma vez, usem duas.

### Driver de validação (`apollo_drv.c`)

O `kernel/drivers/gpu/apollo/apollo_drv.c` é um **driver de validação**, não um driver de
produção: ele se registra no DRM core como um driver completo (load/unload, open/release,
GEM create/free, submit_command, wait/signal fence, mode_set, IRQ handler) para exercitar
de ponta a ponta as APIs nativas recém-implementadas (memória, ring, fence, workqueue,
IRQ). Ele serve de base para definir o formato da compat layer antes de portar o amdgpu.

---

## 3. Camada de Compatibilidade (compat layer)

### Princípios
- **Fina e tipada** — só expõe o que o amdgpu **realmente chama**
- **Nada de `linux/list.h`, `linux/slab.h`, `linux/workqueue.h` vazando** pro kernel
- Wrappers `static inline` em headers privados (`compat/linux_*.h`)
- Mapeamento 1:1 onde possível:
  - `struct mutex` → `spinlock_irq_t` (ou mutex próprio)
  - `struct work_struct` → `work_struct` (já têm workqueue)
  - `dma_fence` → `dma_fence_t` nativo
  - `dma_buf` → `dma_buf_t` nativo
  - `drm_device` → `drm_device_t` nativo

### O que NÃO vai na compat layer
- Memory allocators (`kmalloc`, `vmalloc`, `dma_alloc_coherent`) — usem `kheap`/`vmm`/`pmm` direto
- Locking primitives — usem `spinlock_irq_t`, `mutex_t` nativos
- Lists/queues — usem `list_head` próprio ou macros simples
- Printk — usem `screen_log`/`serial_printf`

### Exemplo de header compat
```c
// kernel/include/compat/linux_dma_fence.h
#pragma once
#include <dma_fence.h>

static inline void dma_fence_init(dma_fence_t *f, const dma_fence_ops_t *ops, spinlock_irq_t *lock, u64 context, u64 seqno) {
    // wrapper pro nativo
    fence_init(f, ops, lock, context, seqno);
}
#define dma_fence_signal(f) fence_signal(f)
#define dma_fence_wait(f, intr) fence_wait(f, intr)
```

---

## 4. Considerações Térmicas e de Segurança

### Camadas de Proteção Térmica (Hardware → Software)

| Camada | Responsável | Temp Típica | Ação |
|--------|-------------|-------------|------|
| **Hardware (ASIC)** | Chip | ~100-110°C | **Hard shutdown** (corta power rails) |
| **Firmware (SMU/VCN)** | VBIOS/SMU | ~90-95°C | Throttle clocks/voltage (DPM) |
| **Driver (amdgpu)** | Kernel | ~80-90°C | Policy: throttle, fan curve, emergency shutdown |
| **Userspace** | Userspace | Configurável | Curva de fan custom, monitoramento |

### O que NÃO existe sem driver funcional
- **Fan control** (pode ficar em 0% ou default BIOS)
- **DPM states** (fica no clock/voltage máximo do VBIOS)
- **Throttle graceful** (só o hard cutoff do ASIC)

### Risco Real no Estágio Atual (sem DPM/fan)
```
Carga leve (modeset, console)  → ~30-50W  → OK na maioria
Carga pesada (compute, 3D)     → 150-300W → Throttle HW ou desliga
```

### Mitigação Prática para Testadores

1. **Forçar power state baixo no init:**
```c
// No amdgpu minimal init
amdgpu_dpm_force_performance_level(adev, AMD_DPM_FORCED_LEVEL_LOW);
```

2. **Desabilitar compute/SDMA rings até ter DPM:**
```c
// Só GFX ring ativo no minimal
adev->gfx.ring[0] = ...;  // GFX
// adev->compute.ring[] = NULL;
// adev->sdma.ring[] = NULL;
```

3. **Monitor térmico obrigatório (kernel thread + serial + overlay):**
```c
// kernel/drivers/gpu/amd/thermal_monitor.c
#define THERMAL_POLL_MS 2000
#define WARN_TEMP_C     85
#define CRIT_TEMP_C     95

static void thermal_monitor_thread(void *arg) {
    while (running) {
        int temp = amdgpu_read_temp(adev);  // MMIO THM register
        if (temp > 0) {
            serial_printf("[THERMAL] GPU: %d°C\n", temp);
            if (temp >= WARN_TEMP_C) screen_draw_text(700, 0, COLOR_RED, "GPU: %d°C HOT!", temp);
            else screen_draw_text(700, 0, COLOR_GREEN, "GPU: %d°C", temp);
            if (temp >= CRIT_TEMP_C) PANIC("GPU overtemp: %d°C", temp);
        }
        timer_sleep_ms(THERMAL_POLL_MS);
    }
}
```

4. **Regra de Ouro para Testes Iniciais:**
> **SÓ modeset + console gráfico.** Sem 3D, sem compute, sem video decode.
> Console 24/7 em Polaris/Vega/Navi = safe com cooler default BIOS.

### iGPU Intel (HD 5500 do testador)
- Risco **zero** — compartilha cooler do CPU
- DPM = pstate do CPU (já têm)
- Thermal trip = package CPU (já monitorado)

---

## 5. Padrão de Teste Visual (GPU Test Pattern)

### Objetivo
Validar visualmente e imediatamente:
- Framebuffer writes funcionam
- Scanout/display pipeline OK
- Modeset correto (resolução, refresh)
- Sem tearing/artifacts
- GPU não trava sob escrita contínua

### Implementação
```c
// kernel/drivers/gpu/gpu_test_pattern.c
// Thread que desenha gradiente HSV animado no framebuffer linear

// HSV animado → RGB565/ARGB8888
// Desenha em fb linear (precisa endereço físico/virtual do BO)
// Roda em thread de baixa prioridade
// Tecla (ex: F12) liga/desliga
```

### Integração
- Compila condicional: `CONFIG_GPU_TEST_PATTERN=y`
- Inicia automaticamente após `polaris_init()` / `amdgpu_modeset_init()`
- Para quando userspace abre `/dev/dri/card0` (evita conflito)

---

## 6. Efeito Multiplicador: Uma Geração Funciona → Todas Beneficiam

### Arquitetura Unificada do amdgpu
```
amdgpu.ko (um .ko só)
├── GCN 1-3 (SI/CIK/KV)     → HD 7xxx, R7/R9 2xx
├── GCN 4-5 (VI/Polaris)    → RX 4xx/5xx, Vega
├── RDNA 1-2 (Navi 1x/2x)   → RX 5xxx/6xxx
└── RDNA 3 (Navi 3x)        → RX 7xxx
```

### Código Compartilhado (funciona em TODAS se Polaris funciona)

| Subsistema | Compartilhado? | Notas |
|------------|----------------|-------|
| `amdgpu_device_init()` | ✅ Sim | Detecta ASIC, aloca estruturas |
| VRAM/GTT managers | ✅ Sim | `amdgpu_vram_mgr`, `amdgpu_gtt_mgr` |
| Display Core (DC) | ✅ Sim | `amdgpu_dm` — genérico, policy por ASIC |
| Firmware loading | ✅ Sim | Só blobs mudam |
| IRQ / GFX/Compute/SDMA rings | ✅ Sim | Mesma API, regs diferentes |
| PowerPlay (DPM) | ⚠️ Parcial | Tabelas por ASIC, mas framework comum |

### O que É Específico por Geração (~2-3k LOC cada)
- Device ID tables / PCI quirks
- Register offsets (mesmo IP, endereços diferentes)
- PowerPlay tables (clocks, voltagens, limits)
- DC link encoder/transmitter params
- Firmware version requirements

### Conclusão Prática
> Se **Polaris (VI)** roda **modeset + VRAM + DC + suspend** na compat layer →
> **Vega, Navi 1/2 já bootam com console gráfico**.
> Só testando em HW real descobrem os quirks restantes.

---

## 7. Coordenação de Equipe (4 Devs)

> Divisão operacional detalhada: `GPU_PORTING_TASKS.md` (4 devs × 5 fases, 1 tarefa por dev por fase).

### Alocação por Fase

| Fase | Esforço | Foco |
|------|---------|------|
| **1 — Infra DRM + Memória (Mês 1)** | 4 × 1 tarefa | DRM core, GEM, dma-fence/resv, firmware + PCIe |
| **2 — Scheduler / Atomic KMS / Compat (Mês 1-2)** | 4 × 1 tarefa | dma-buf/PRIME, drm_sched, atomic KMS, compat layer |
| **3 — amdgpu v0.1.0 MINIMAL (Mês 2-3)** | 4 × 1 tarefa | HW init/ASIC, VRAM/GTT mgr, modeset fbdev, GFX ring + thermal |
| **4 — Display Core (Mês 3-5)** | 4 × 1 tarefa | DCE 8/10/11, DCN 1/2, link/MST, atomic-DC |
| **5 — DPM/Compute/Validação (Mês 5-6)** | 4 × 1 tarefa | PowerPlay, compute/SDMA, testes/CI, milestones + i915 |

### Sem devs dedicados a manutenção
- Não há "manutenção kernel base" separada: os 4 devs rodam o roadmap.
- Gaps do kernel base que **bloqueiam** a fase (ex.: `#PF/demand paging`, scheduler preemptivo, workqueue — ver `KERNEL_TODO.md`) são resolvidos pelo dev que ficou bloqueado, como prioridade da fase.
- O tech lead mantém o gateway de review em `compat/` e `drm/`; cada tarefa tem um **second reviewer** designado entre os outros 3 devs.

### Rotatividade / Bus factor
- A cada 2 fases, **trocar as tarefas entre devs** (rotação de foco código ↔ testes) para espalhar conhecimento — crítico entre quem fizeram DC e quem fizeram core.
- Documentação viva (este .md + `GPU_PORTING_TASKS.md` + `KERNEL_TODO.md`) é obrigatória: sem ela a rotação vira retrabalho.

### Governança Técnica
- **Tech Lead único** para compat layer + DRM core (define interfaces, aprova PRs)
- **Code review obrigatório** — nenhum commit direto em `compat/` ou `drm/`
- **Issue template** no GitHub com campos: `lspci -vv`, `dmesg`, `GPU model`, `repro steps`
- **Milestones** no GitHub: `Infra DRM`, `amdgpu Minimal`, `DC Polaris`, `DC Vega`, `DPM`

---

## 8. Riscos e Mitigações

| Risco | Probabilidade | Impacto | Mitigação |
|-------|---------------|---------|-----------|
| DC muito complexo para portar em 2 meses | Alta | Atraso crítico | Começar minimal **sem DC** (fbdev linear); DC vem depois |
| Firmware blobs não redistribuíveis | Média | Usuário não consegue testar | Incluir firmware built-in no kernel (licença AMD permite) |
| Testadores queimam GPU (sem DPM) | Baixa | Hardware danificado | Thermal monitor obrigatório + power state forçado LOW |
| Compat layer vaza abstrações Linux | Média | Kernel fica "Linux-like" | Code review estrito; headers compat privados; static analysis |
| Bus factor no tech lead GPU | Média | Projeto para se ele sai | Documentação viva (este .md + `GPU_PORTING_TASKS.md`); second reviewer por tarefa; rotação de tarefas a cada 2 fases |
| i915 futuro quebra DRM core | Baixa | Retrabalho | DRM core desenhado genérico desde o dia 1 |

---

## 9. Próximos Passos Imediatos (Action Items)

> **Fase 1 e Fase 2 concluídas** (itens 1-9 + DRM core). Próximo bloco: Fase 3 — amdgpu v0.1.0 MINIMAL.

1. **[x]** Criar `kernel/drivers/drm/drm_device.c` — DRM core real, ioctl dispatch, `/dev/dri/card0` (*Fase 1 Dev 1 — entregue*)
2. **[x]** Criar `kernel/drivers/drm/drm_gem.c` — GEM minimal (BO linear, refcount, mmap) (*Fase 1 Dev 2 — entregue*)
3. **[x]** Criar `kernel/drivers/drm/dma_fence.c` — fence + callback + timeline (*Fase 1 Dev 3 — entregue*)
4. **[x]** Criar `kernel/drivers/drm/dma_resv.c` — reservation object (*Fase 1 Dev 3 — entregue*)
5. **[x]** Criar `kernel/fs/firmware.c` — `request_firmware()` + built-in fallback (*Fase 1 Dev 4 — entregue, em validação*)
6. **[x]** Criar `kernel/drivers/drm/dma_buf.c` — export/import, mmap, sync_file (*Fase 2 Dev 1 — entregue*)
7. **[x]** Criar `kernel/drivers/drm/drm_sched.c` — job queue + entity + runqueue + timeout/recovery (*Fase 2 Dev 2 — entregue*)
8. **[x]** Criar `kernel/drivers/drm/drm_atomic.c` — atomic KMS base com rollback (*Fase 2 Dev 3 — entregue*)
9. **[x]** Criar `kernel/include/compat/linux_*.h` — headers da compat layer (*Fase 2 Dev 4 — entregue*)
10. **[ ]** Completar MSI-X allocation dinâmica + IOMMU real (DMAR via ACPI)
11. **[ ]** Adicionar `thermal_monitor.c` no `polaris.c` / futuro `amdgpu` (*Fase 3 Dev 4*)
12. **[ ]** Adicionar `gpu_test_pattern.c` condicional (*`polaris_test_pattern` já existe como ponto de partida; Fase 3 Dev 4*)
13. **[ ]** Definir baseline LTS exata (tag do kernel Linux) e travar
14. **[ ]** Criar milestone `amdgpu Minimal` no GitHub com issues por arquivo (Fase 3)

---

## 10. Referências Úteis

- **Linux DRM Developer Guide:** https://drm.freedesktop.org/docs/drm/
- **amdgpu Source (6.6):** `drivers/gpu/drm/amd/`
- **FreeBSD linuxkpi:** `sys/compat/linuxkpi/` — exemplo maduro de compat layer
- **AMD GPU Register Docs:** https://www.amd.com/en/developer/gpu-open.html
- **DRM/KMS Atomic Tutorial:** https://blog.ffwll.ch/2020/01/atomic-modesetting.html

---

*Documento vivo — atualizar a cada milestone concluída.*