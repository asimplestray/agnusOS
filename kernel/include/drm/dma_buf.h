/*
 * drm/dma_buf.h — dma-buf + PRIME + sync_file nativos.
 *
 * Fase 2 Dev 1 — entrega: BO exportada por um driver e importada por
 * outro (2 devices fake) com mmap compartilhado; sync_file baseado em
 * dma_fence para transferência de exclusão mútua entre dispositivos.
 *
 * Modelo (espelha o Linux, sem fd):
 *   - O exportador cria um dma_buf com ops + storage próprio
 *     (tipicamente um GEM BO via drm_gem_prime_export()).
 *   - O importador faz attach() do dma_buf ao seu drm_device e usa
 *     dma_buf_vmap() para mapear o MESMO armazenamento no espaço de
 *     kernel (mmap compartilhado entre os dois drivers).
 *   - A sincronização vai pelo dma_resv embutido: fences de leitura/
 *     escrita anexadas pelo exportador são vistas pelos importadores.
 */

#ifndef _DRM_DMA_BUF_H_
#define _DRM_DMA_BUF_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <spinlock.h>
#include <drm/dma_resv.h>

struct dma_buf;
struct drm_device;

/* Flags de exportação */
#define DMA_BUF_FLAG_READONLY (1 << 0)

/* Ops fornecidas pelo exportador */
struct dma_buf_ops {
    /* Libera recursos do exportador quando o dma_buf morre (último put). */
    void (*release)(struct dma_buf *dmabuf);

    /* Lista de páginas físicas espalhadas (GTT). Preencher UM dos dois. */
    int  (*get_pages)(struct dma_buf *dmabuf, uint64_t **pages_out,
                      size_t *num_pages_out);

    /* Base física contígua (VRAM/carveout). */
    int  (*get_phys)(struct dma_buf *dmabuf, uint64_t *phys_out);

    /* Nome curto do exportador para debug. */
    const char *(*exp_name)(struct dma_buf *dmabuf);
};

/* Anexo de um dma_buf a um dispositivo importador */
struct dma_buf_attachment {
    struct dma_buf           *buf;
    struct drm_device        *dev;      /* importador */
    void                     *vaddr;    /* janela de kernel mapeada ou NULL */
    struct dma_buf_attachment *next;
};

/* O buffer compartilhado em si */
struct dma_buf {
    size_t                    size;
    uint32_t                  flags;
    uint32_t                  refcount;
    spinlock_irq_t            ref_lock;
    const struct dma_buf_ops  *ops;
    void                      *priv;            /* storage do exportador */
    struct dma_resv           *resv;            /* sincronização (owned) */
    struct dma_buf_attachment *attachments;
    spinlock_irq_t            attach_lock;
};

/* ---- Ciclo de vida ---- */
struct dma_buf *dma_buf_export(const struct dma_buf_ops *ops, void *priv,
                               size_t size, uint32_t flags);
struct dma_buf *dma_buf_get(struct dma_buf *dmabuf);
void            dma_buf_put(struct dma_buf *dmabuf);
uint32_t        dma_buf_refcount(struct dma_buf *dmabuf);

/* Reserva object embutida (nunca NULL após export) */
struct dma_resv *dma_buf_resv(struct dma_buf *dmabuf);

/* ---- Importação (PRIME) ---- */
struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
                                          struct drm_device *importer_dev);
void dma_buf_detach(struct dma_buf_attachment *attach);

/* ---- CPU access (mmap compartilhado entre exportador e importador) ---- */
void *dma_buf_vmap(struct dma_buf_attachment *attach);
void  dma_buf_vunmap(struct dma_buf_attachment *attach);

/* ---- sync_file: fence empacotada p/ transferência entre devices ---- */
struct sync_file {
    uint32_t         refcount;
    spinlock_irq_t   ref_lock;
    struct dma_fence *fence;    /* owned ref */
    char             name[32];
};

struct sync_file  *sync_file_create(struct dma_fence *fence, const char *name);
void               sync_file_put(struct sync_file *sf);
struct dma_fence  *sync_file_get_fence(struct sync_file *sf); /* nova referência */
int                sync_file_wait_timeout(struct sync_file *sf, bool intr,
                                          int64_t timeout_ns);

/* ---- GEM PRIME glue (exporta um GEM BO como dma_buf) ---- */
struct drm_gem_object;
struct dma_buf *drm_gem_prime_export(struct drm_gem_object *bo,
                                     uint32_t flags);

/* ---- Selftest de boot (2 devices fake, mmap compartilhado) ---- */
int dma_buf_test(void);

#endif /* _DRM_DMA_BUF_H_ */
