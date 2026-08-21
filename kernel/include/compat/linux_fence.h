/*
 * compat/linux_fence.h — dma-fence estilo Linux sobre a nativa.
 *
 * Fase 2 Dev 4 — Camada de Compatibilidade.
 *
 * Mapeamento 1:1 (ver GPU_PORTING_STRATEGY.md seção 3):
 *   dma_fence     -> struct dma_fence nativo (<drm/dma_fence.h>)
 *   dma_resv      -> struct dma_resv  nativo (<drm/dma_resv.h>)
 *
 * A API nativa já usa os nomes do Linux (dma_fence_init/signal/wait/
 * add_callback, dma_resv_add_excl_fence, ...). Este header fornece
 * somente helpers ausentes e o ponto único de inclusão para drivers.
 */

#ifndef _COMPAT_LINUX_FENCE_H_
#define _COMPAT_LINUX_FENCE_H_

#include <drm/dma_fence.h>
#include <drm/dma_resv.h>

/* Linux: dma_fence_context_alloc(n) — aloca n contextos únicos */
static inline uint64_t dma_fence_context_alloc(unsigned num)
{
    static volatile uint64_t next_context;
    return __sync_fetch_and_add(&next_context, (uint64_t)num);
}

/* Linux: dma_fence_wait_any(fences,count,intr) sem timeout */
static inline int dma_fence_wait_any(struct dma_fence **fences, uint32_t count,
                                     bool intr)
{
    return dma_fence_wait_any_timeout(fences, count, intr, -1, NULL);
}

#endif /* _COMPAT_LINUX_FENCE_H_ */
