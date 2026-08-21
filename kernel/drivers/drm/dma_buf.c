/*
 * dma_buf.c — dma-buf + PRIME + sync_file nativos.
 *
 * Fase 2 Dev 1. O armazenamento pertence ao exportador (ex.: GEM BO);
 * importadores apenas anexam e mapeiam as mesmas páginas físicas.
 */

#include <drm/dma_buf.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <pmm.h>
#include <vmm.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>
#include <screen.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

/* Janela de vmap exclusiva do dma-buf (separada da janela do GEM) */
#define DMA_BUF_VMAP_BASE 0xFFFF800500000000ULL
#define DMA_BUF_VMAP_SIZE (1ULL << 30)

static uint64_t dmabuf_vmap_next = DMA_BUF_VMAP_BASE;

static void dmabuf_log(const char *tag, vga_color_t color, const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_print("[DMA-BUF] ");
    serial_print(buf);
    serial_print("\n");
    screen_log(tag, color, buf);
}

/* ------------------------------------------------------------------ */
/* Ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

struct dma_buf *dma_buf_export(const struct dma_buf_ops *ops, void *priv,
                               size_t size, uint32_t flags)
{
    struct dma_buf *dmabuf;

    if (!ops || !ops->release || size == 0)
        return NULL;

    /* Exportador precisa expor páginas OU base física contígua */
    if (!ops->get_pages && !ops->get_phys)
        return NULL;

    dmabuf = kmalloc(sizeof(struct dma_buf));
    if (!dmabuf)
        return NULL;
    memset(dmabuf, 0, sizeof(*dmabuf));

    dmabuf->size = size;
    dmabuf->flags = flags;
    dmabuf->ops = ops;
    dmabuf->priv = priv;
    dmabuf->refcount = 1;
    spinlock_init(&dmabuf->ref_lock.lock);

    dmabuf->resv = kmalloc(sizeof(struct dma_resv));
    if (!dmabuf->resv) {
        kfree(dmabuf);
        return NULL;
    }
    dma_resv_init(dmabuf->resv);
    dmabuf->resv->owner = dmabuf;

    spinlock_init(&dmabuf->attach_lock.lock);

    dmabuf_log("INFO", COLOR_LIGHT_CYAN, "exported size=%u", (unsigned)size);
    return dmabuf;
}

struct dma_buf *dma_buf_get(struct dma_buf *dmabuf)
{
    if (!dmabuf)
        return NULL;
    unsigned long flags;
    spin_lock_irqsave(&dmabuf->ref_lock, &flags);
    dmabuf->refcount++;
    spin_unlock_irqrestore(&dmabuf->ref_lock, flags);
    return dmabuf;
}

void dma_buf_put(struct dma_buf *dmabuf)
{
    uint32_t n;

    if (!dmabuf)
        return;

    unsigned long flags;
    spin_lock_irqsave(&dmabuf->ref_lock, &flags);
    n = --dmabuf->refcount;
    spin_unlock_irqrestore(&dmabuf->ref_lock, flags);

    if (n != 0)
        return;

    /* Última referência: nenhum attachment pode sobrar */
    if (dmabuf->attachments) {
        dmabuf_log("WARN", COLOR_BROWN,
                   "destroyed with live attachments (leak)");
    }

    if (dmabuf->ops && dmabuf->ops->release)
        dmabuf->ops->release(dmabuf);

    dma_resv_fini(dmabuf->resv);
    kfree(dmabuf->resv);
    kfree(dmabuf);
}

uint32_t dma_buf_refcount(struct dma_buf *dmabuf)
{
    if (!dmabuf)
        return 0;
    unsigned long flags;
    spin_lock_irqsave(&dmabuf->ref_lock, &flags);
    uint32_t n = dmabuf->refcount;
    spin_unlock_irqrestore(&dmabuf->ref_lock, flags);
    return n;
}

struct dma_resv *dma_buf_resv(struct dma_buf *dmabuf)
{
    return dmabuf ? dmabuf->resv : NULL;
}

/* ------------------------------------------------------------------ */
/* Importação                                                          */
/* ------------------------------------------------------------------ */

struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
                                          struct drm_device *importer_dev)
{
    struct dma_buf_attachment *att;

    if (!dmabuf || !importer_dev)
        return NULL;

    att = kmalloc(sizeof(struct dma_buf_attachment));
    if (!att)
        return NULL;
    memset(att, 0, sizeof(*att));
    att->buf = dmabuf;
    att->dev = importer_dev;

    unsigned long flags;
    spin_lock_irqsave(&dmabuf->attach_lock, &flags);
    att->next = dmabuf->attachments;
    dmabuf->attachments = att;
    spin_unlock_irqrestore(&dmabuf->attach_lock, flags);

    return att;
}

void dma_buf_detach(struct dma_buf_attachment *attach)
{
    struct dma_buf *dmabuf;
    struct dma_buf_attachment **p;

    if (!attach)
        return;
    dmabuf = attach->buf;
    if (!dmabuf)
        return;

    if (attach->vaddr)
        dma_buf_vunmap(attach);

    unsigned long flags;
    spin_lock_irqsave(&dmabuf->attach_lock, &flags);
    p = &dmabuf->attachments;
    while (*p) {
        if (*p == attach) {
            *p = attach->next;
            spin_unlock_irqrestore(&dmabuf->attach_lock, flags);
            kfree(attach);
            return;
        }
        p = &(*p)->next;
    }
    spin_unlock_irqrestore(&dmabuf->attach_lock, flags);
}

/* ------------------------------------------------------------------ */
/* CPU access — mapeia o MESMO armazenamento físico                    */
/* ------------------------------------------------------------------ */

void *dma_buf_vmap(struct dma_buf_attachment *attach)
{
    struct dma_buf *dmabuf;
    uint64_t win;
    size_t num_pages;

    if (!attach || !attach->buf)
        return NULL;
    dmabuf = attach->buf;

    if (attach->vaddr)
        return attach->vaddr;   /* já mapeado */

    num_pages = (dmabuf->size + PAGE_SIZE - 1) / PAGE_SIZE;

    unsigned long flags;
    spin_lock_irqsave(&dmabuf->attach_lock, &flags);

    win = __sync_fetch_and_add(&dmabuf_vmap_next,
                               (uint64_t)(num_pages * PAGE_SIZE));
    if (win + num_pages * PAGE_SIZE > DMA_BUF_VMAP_BASE + DMA_BUF_VMAP_SIZE) {
        spin_unlock_irqrestore(&dmabuf->attach_lock, flags);
        dmabuf_log("ERROR", COLOR_RED, "vmap window exhausted");
        return NULL;
    }

    uint64_t *pages = NULL;
    size_t npages_out = 0;
    uint64_t contig = 0;

    if (dmabuf->ops->get_pages &&
        dmabuf->ops->get_pages(dmabuf, &pages, &npages_out) == 0 &&
        pages && npages_out > 0) {
        for (size_t i = 0; i < npages_out; i++)
            vmm_map_page(win + i * PAGE_SIZE, pages[i],
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    } else if (dmabuf->ops->get_phys &&
               dmabuf->ops->get_phys(dmabuf, &contig) == 0 && contig) {
        vmm_map_region(win, contig, (uint32_t)dmabuf->size,
                       VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    } else {
        spin_unlock_irqrestore(&dmabuf->attach_lock, flags);
        dmabuf_log("ERROR", COLOR_RED, "exporter exposes no storage");
        return NULL;
    }

    (void)num_pages;
    attach->vaddr = (void *)(uintptr_t)win;
    spin_unlock_irqrestore(&dmabuf->attach_lock, flags);

    dmabuf_log("INFO", COLOR_LIGHT_CYAN, "attachment mapped at %x:%x",
               (unsigned)((win >> 32) & 0xFFFFFFFF),
               (unsigned)(win & 0xFFFFFFFF));
    return attach->vaddr;
}

void dma_buf_vunmap(struct dma_buf_attachment *attach)
{
    struct dma_buf *dmabuf;
    uint64_t win;
    size_t num_pages;

    if (!attach || !attach->vaddr)
        return;
    dmabuf = attach->buf;
    win = (uint64_t)(uintptr_t)attach->vaddr;
    num_pages = (dmabuf->size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < num_pages; i++)
        vmm_unmap_page(win + i * PAGE_SIZE);

    attach->vaddr = NULL;
}

/* ------------------------------------------------------------------ */
/* sync_file                                                           */
/* ------------------------------------------------------------------ */

struct sync_file *sync_file_create(struct dma_fence *fence, const char *name)
{
    struct sync_file *sf;

    if (!fence)
        return NULL;

    sf = kmalloc(sizeof(struct sync_file));
    if (!sf)
        return NULL;
    memset(sf, 0, sizeof(*sf));

    sf->refcount = 1;
    spinlock_init(&sf->ref_lock.lock);
    sf->fence = dma_fence_get(fence);
    if (name) {
        strncpy(sf->name, name, sizeof(sf->name) - 1);
        sf->name[sizeof(sf->name) - 1] = '\0';
    } else {
        strncpy(sf->name, "sync_file", sizeof(sf->name) - 1);
    }

    return sf;
}

void sync_file_put(struct sync_file *sf)
{
    uint32_t n;

    if (!sf)
        return;

    unsigned long flags;
    spin_lock_irqsave(&sf->ref_lock, &flags);
    n = --sf->refcount;
    spin_unlock_irqrestore(&sf->ref_lock, flags);

    if (n == 0) {
        dma_fence_put(sf->fence);
        kfree(sf);
    }
}

struct dma_fence *sync_file_get_fence(struct sync_file *sf)
{
    if (!sf)
        return NULL;
    return dma_fence_get(sf->fence);
}

int sync_file_wait_timeout(struct sync_file *sf, bool intr, int64_t timeout_ns)
{
    if (!sf || !sf->fence)
        return -EINVAL;
    return dma_fence_wait_timeout(sf->fence, intr, timeout_ns);
}

/* ------------------------------------------------------------------ */
/* GEM PRIME glue: exporta um GEM BO como dma_buf                      */
/* ------------------------------------------------------------------ */

/* ops->release: solta a referência que o export pegou no BO */
static void gem_prime_release(struct dma_buf *dmabuf)
{
    gem_put((struct drm_gem_object *)dmabuf->priv);
}

static int gem_prime_get_pages(struct dma_buf *dmabuf, uint64_t **pages_out,
                               size_t *num_pages_out)
{
    struct drm_gem_object *bo = dmabuf->priv;

    if (bo->domain != DRM_GEM_DOMAIN_GTT || !bo->pages)
        return -EINVAL;
    *pages_out = bo->pages;         /* emprestado; BO vivo via ref */
    *num_pages_out = bo->num_pages;
    return 0;
}

static int gem_prime_get_phys(struct dma_buf *dmabuf, uint64_t *phys_out)
{
    struct drm_gem_object *bo = dmabuf->priv;

    if (bo->domain != DRM_GEM_DOMAIN_VRAM)
        return -EINVAL;
    *phys_out = bo->phys;
    return 0;
}

static const char *gem_prime_exp_name(struct dma_buf *dmabuf)
{
    (void)dmabuf;
    return "gem_prime";
}

static const struct dma_buf_ops gem_prime_ops = {
    .release   = gem_prime_release,
    .get_pages = gem_prime_get_pages,
    .get_phys  = gem_prime_get_phys,
    .exp_name  = gem_prime_exp_name,
};

struct dma_buf *drm_gem_prime_export(struct drm_gem_object *bo, uint32_t flags)
{
    if (!bo)
        return NULL;
    /* Referência mantida enquanto o dma_buf existir (solta em release) */
    gem_get(bo);
    struct dma_buf *dmabuf = dma_buf_export(&gem_prime_ops, bo, bo->size, flags);
    if (!dmabuf)
        gem_put(bo);
    return dmabuf;
}

/* ------------------------------------------------------------------ */
/* Selftest de boot: 2 devices fake, BO exportada por A e importada    */
/* por B com mmap compartilhado + sync_file sobre o resv.              */
/* ------------------------------------------------------------------ */

#define DMABUF_TEST_BO_SIZE (4 * PAGE_SIZE)

int dma_buf_test(void)
{
    struct drm_device dev_a;    /* exportador */
    struct drm_device dev_b;    /* importador */
    struct drm_gem_object *bo = NULL;
    struct dma_buf *dmabuf = NULL;
    struct dma_buf_attachment *att = NULL;
    struct sync_file *sf = NULL;
    struct dma_fence *fence = NULL;
    spinlock_irq_t fence_lock;
    size_t shrink_before;
    int rc = -EINVAL;
    uint32_t *va;
    int failures = 0;

    memset(&dev_a, 0, sizeof(dev_a));
    memset(&dev_b, 0, sizeof(dev_b));

    serial_print("[DMA-BUF] Starting dma-buf/PRIME tests...\n");

    shrink_before = drm_gem_shrinker_count();

    /* Dois devices só com domínio GTT (sem carveout VRAM) */
    drm_gem_init(&dev_a, 0, 0);
    drm_gem_init(&dev_b, 0, 0);

    /* A cria a BO e escreve um padrão conhecido */
    if (gem_create(&dev_a, DMABUF_TEST_BO_SIZE, DRM_GEM_DOMAIN_GTT, &bo)) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "gem_create on device A failed");
        goto out;
    }
    va = gem_mmap_phys(bo);
    if (!va) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "exporter mmap failed");
        goto out;
    }
    for (uint32_t i = 0; i < DMABUF_TEST_BO_SIZE / 4; i++)
        va[i] = 0xCAFE0000u + i;

    /* Exporta da perspectiva de A */
    dmabuf = drm_gem_prime_export(bo, 0);
    if (!dmabuf || !dmabuf->resv) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "prime export failed");
        goto out;
    }
    if (dma_buf_refcount(dmabuf) != 1 || dmabuf->size != DMABUF_TEST_BO_SIZE) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "bad export metadata");
        goto out;
    }
    dmabuf_log("INFO", COLOR_LIGHT_CYAN,
               "BO exported by driver A (%s)", dmabuf->ops->exp_name(dmabuf));

    /* Importa em B e verifica mmap compartilhado */
    att = dma_buf_attach(dmabuf, &dev_b);
    if (!att) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "attach on device B failed");
        goto out;
    }
    void *vb = dma_buf_vmap(att);
    if (!vb) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "importer vmap failed");
        goto out;
    }

    bool ok = true;
    uint32_t *vb32 = vb;
    for (uint32_t i = 0; i < DMABUF_TEST_BO_SIZE / 4; i++) {
        if (vb32[i] != 0xCAFE0000u + i) { ok = false; break; }
    }
    if (!ok) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "shared mmap: B does not see A data");
        goto out;
    }
    dmabuf_log("PASS", COLOR_LIGHT_GREEN,
               "mmap compartilhado OK (B le padrao escrito por A)");

    /* Escreve de B e confere via mapeamento de A (bidirecional) */
    vb32[0] = 0xBEEF1234u;
    if (va[0] != 0xBEEF1234u) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "write from B not visible at A");
        goto out;
    }
    dmabuf_log("PASS", COLOR_LIGHT_GREEN,
               "escrita do importador visivel no exportador");

    /* Sincronização: fence exclusiva no resv + sync_file */
    spinlock_init(&fence_lock.lock);
    fence = kmalloc(sizeof(struct dma_fence));
    if (!fence) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "fence alloc failed");
        rc = -ENOMEM;
        goto out;
    }
    dma_fence_init(fence, &dma_fence_default_ops, &fence_lock, 777, 1);

    if (dma_resv_add_excl_fence(dmabuf->resv, fence)) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "resv add excl fence failed");
        goto out;
    }

    sf = sync_file_create(fence, "dmabuf_test");
    if (!sf) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "sync_file_create failed");
        goto out;
    }
    if (sync_file_wait_timeout(sf, false, 0) != -ETIMEDOUT) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "unsignaled sync_file should timeout");
        goto out;
    }
    dma_fence_signal(fence);
    if (sync_file_wait_timeout(sf, false, 0) != 0) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "signaled sync_file should pass");
        goto out;
    }
    {
        struct dma_fence *got = sync_file_get_fence(sf);
        if (got != fence) {
            dmabuf_log("FAIL", COLOR_LIGHT_RED, "get_fence mismatch");
            dma_fence_put(got);
            goto out;
        }
        dma_fence_put(got);
    }
    dmabuf_log("PASS", COLOR_LIGHT_GREEN,
               "sync_file wait/signal/get_fence OK sobre resv do dma-buf");

    /* Cleanup ordenado */
    dma_buf_vunmap(att);
    dma_buf_detach(att);
    att = NULL;
    if (dmabuf->attachments != NULL) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "detach did not unlink attachment");
        goto out;
    }
    sync_file_put(sf);
    sf = NULL;
    dma_buf_put(dmabuf);   /* solta BO de A (release -> gem_put) */
    dmabuf = NULL;
    gem_unmap_phys(bo);
    gem_put(bo);           /* destrói a BO */
    bo = NULL;

    if (drm_gem_shrinker_count() != shrink_before) {
        dmabuf_log("FAIL", COLOR_LIGHT_RED, "storage leak after teardown");
        goto out;
    }

    dmabuf_log("PASS", COLOR_LIGHT_GREEN,
               "dma-buf/PRIME: ALL CHECKS PASSED");
    rc = 0;

out:
    if (sf)
        sync_file_put(sf);
    if (fence)
        dma_fence_put(fence);
    if (att)
        dma_buf_detach(att);
    if (dmabuf)
        dma_buf_put(dmabuf);
    if (bo) {
        gem_unmap_phys(bo);
        gem_put(bo);
    }
    drm_gem_fini(&dev_a);
    drm_gem_fini(&dev_b);

    if (rc != 0 && failures == 0)
        failures = 1;
    return rc;
}
