/*
 * compat/linux_dma_buf.h — dma-buf/sync_file estilo Linux sobre o nativo.
 *
 * Fase 2 Dev 4 — Camada de Compatibilidade.
 *
 * Mapeamento 1:1 (ver GPU_PORTING_STRATEGY.md seção 3):
 *   dma_buf    -> struct dma_buf    nativo (<drm/dma_buf.h>, Fase 2 Dev 1)
 *   sync_file  -> struct sync_file  nativo (<drm/dma_buf.h>)
 *
 * A implementação nativa já segue a nomenclatura Linux
 * (dma_buf_export/get/put/attach/vmap, sync_file_create/...).
 */

#ifndef _COMPAT_LINUX_DMA_BUF_H_
#define _COMPAT_LINUX_DMA_BUF_H_

#include <drm/dma_buf.h>

/* Linux: get_dma_buf(dmabuf) — pega referência com semântica fd */
#define get_dma_buf(dmabuf) dma_buf_get(dmabuf)

/* Linux: dma_buf_get_attachment helper fino */
static inline struct dma_resv *get_dma_buf_resv(struct dma_buf *dmabuf)
{
    return dma_buf_resv(dmabuf);
}

#endif /* _COMPAT_LINUX_DMA_BUF_H_ */
