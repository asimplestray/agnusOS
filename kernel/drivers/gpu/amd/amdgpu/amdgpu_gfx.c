/*
 * amdgpu_gfx.c — GFX ring (CP) minimal da v0.1.0 (Dev 4).
 *
 * Ring buffer em BO GTT + submissão pelos regs GFX_RB_WPTR/RPTR,
 * integrado ao DRM Scheduler da Fase 2 (jobs → run_job → fence).
 *
 * A emulação vgpu não implementa um CP de verdade: após gravar os
 * pacotes no ring e publicar o WPTR, o driver drena o ring em CPU
 * executando as escritas na VRAM e sinalizando fences — o mesmo
 * contrato que IRQ EOP cumpre no hardware real. Quando o emulador
 * ganhar decodificação PM4 real, este pump sai sem mudar a API.
 *
 * Formato dos pacotes (header PACKET3-style):
 *   dword0 = 0xC0000000 | (count << 16) | (opcode << 8)
 *   NOP        (0x10): ignora `count` dwords extras
 *   WRITE_DATA (0x37): cntl, addr_lo, addr_hi, value   (count==3)
 *   FENCE      (0x99): seq_lo, seq_hi                  (count==1;
 *                     opcode exclusivo da emulação — no HW real o
 *                     sinal vem de EVENT_WRITE_EOP + IRQ)
 */

#include <amdgpu.h>
#include <drm/drm_gem.h>
#include <drm/drm_sched.h>
#include <compat/linux_types.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>
#include <screen.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

#define RING_MASK (AMDGPU_RING_SIZE_DW - 1)

/* Fences aguardando consumo pelo pump */
#define GFX_PENDING_MAX 16

struct gfx_pending {
    uint64_t          key;      /* seqno embutido no pacote */
    struct dma_fence *fence;    /* referência owned         */
};

static struct gfx_pending gfx_pend[GFX_PENDING_MAX];
static int gfx_pend_count;

static struct dma_fence *gfx_run_job(struct drm_sched_job *sjob);
static void gfx_timedout_job(struct drm_sched_job *sjob);

static const struct drm_sched_ops gfx_sched_ops = {
    .run_job      = gfx_run_job,
    .timedout_job = gfx_timedout_job,
    .free_job     = NULL,
};

uint32_t amdgpu_gfx_rptr(struct amdgpu_device *adev)
{
    return amdgpu_rreg(adev, mmGFX_RB_RPTR);
}

/* ------------------------------------------------------------------ */
/* Ring                                                                */
/* ------------------------------------------------------------------ */

static void ring_put_dw(struct amdgpu_device *adev, uint32_t dw)
{
    adev->ring_cpu[adev->ring_wptr] = dw;
    adev->ring_wptr = (adev->ring_wptr + 1) & RING_MASK;
}

int amdgpu_gfx_init(struct amdgpu_device *adev)
{
    int rc;

    if (!adev)
        return -EINVAL;

    /* Ring em GTT (RAM do sistema) */
    rc = amdgpu_bo_create(adev,
                          AMDGPU_RING_SIZE_DW * sizeof(uint32_t),
                          false, &adev->ring_bo);
    if (rc) {
        adev_log("FAIL", COLOR_LIGHT_RED, "BO do ring falhou (%d)", rc);
        return rc;
    }
    adev->ring_cpu = gem_mmap_phys(adev->ring_bo);
    if (!adev->ring_cpu) {
        gem_put(adev->ring_bo);
        adev->ring_bo = NULL;
        return -ENOMEM;
    }
    memset(adev->ring_cpu, 0, AMDGPU_RING_SIZE_DW * sizeof(uint32_t));
    adev->ring_wptr = 0;
    gfx_pend_count = 0;

    /* Regs CP zerados (na emulação são storage puro) */
    amdgpu_wreg(adev, mmGFX_RB_RPTR, 0);
    amdgpu_wreg(adev, mmGFX_RB_WPTR, 0);

    /* Scheduler DRM para os jobs do ring */
    rc = drm_sched_init(&adev->sched, &gfx_sched_ops, "amdgpu-gfx", 100);
    if (rc) {
        gem_unmap_phys(adev->ring_bo);
        gem_put(adev->ring_bo);
        adev->ring_cpu = NULL;
        adev->ring_bo = NULL;
        return rc;
    }
    rc = drm_sched_entity_init(&adev->entity, &adev->sched);
    if (rc) {
        drm_sched_fini(&adev->sched);
        gem_unmap_phys(adev->ring_bo);
        gem_put(adev->ring_bo);
        adev->ring_cpu = NULL;
        adev->ring_bo = NULL;
        return rc;
    }

    adev_log("INFO", COLOR_LIGHT_CYAN,
             "GFX ring pronto (%u dwords, compute/SDMA desligados)",
             AMDGPU_RING_SIZE_DW);
    return 0;
}

void amdgpu_gfx_fini(struct amdgpu_device *adev)
{
    if (!adev)
        return;

    drm_sched_fini(&adev->sched);

    if (adev->ring_cpu && adev->ring_bo) {
        gem_unmap_phys(adev->ring_bo);
        gem_put(adev->ring_bo);
    }
    adev->ring_cpu = NULL;
    adev->ring_bo = NULL;

    for (int i = 0; i < gfx_pend_count; i++)
        dma_fence_put(gfx_pend[i].fence);
    gfx_pend_count = 0;
}

/* ------------------------------------------------------------------ */
/* Pump — consome pacotes do ring                                      */
/* ------------------------------------------------------------------ */

static uint32_t gfx_fetch(struct amdgpu_device *adev, uint32_t rptr,
                          uint32_t idx)
{
    return adev->ring_cpu[(rptr + idx) & RING_MASK];
}

static void gfx_signal_key(uint64_t key)
{
    for (int i = 0; i < gfx_pend_count; i++) {
        if (gfx_pend[i].key == key) {
            dma_fence_signal(gfx_pend[i].fence);
            dma_fence_put(gfx_pend[i].fence);
            gfx_pend[i] = gfx_pend[gfx_pend_count - 1];
            gfx_pend_count--;
            return;
        }
    }
}

static bool gfx_addr_in_vram(struct amdgpu_device *adev, uint64_t gaddr)
{
    return gaddr >= adev->vram_phys &&
           gaddr + 4 <= adev->vram_phys + adev->vram_size;
}

void amdgpu_gfx_drain(struct amdgpu_device *adev)
{
    uint32_t rptr = amdgpu_gfx_rptr(adev);
    uint32_t guard = 0;
    static bool warned_bad_addr;

    while (rptr != adev->ring_wptr && guard++ < AMDGPU_RING_SIZE_DW) {
        uint32_t hdr = adev->ring_cpu[rptr];
        uint32_t opcode = (hdr >> 8) & 0xFF;
        uint32_t count = (hdr >> 16) & 0xFFF;

        switch (opcode) {
        case AMDGPU_GFX_OPCODE_NOP:
            rptr = (rptr + 1 + count) & RING_MASK;
            break;

        case AMDGPU_GFX_OPCODE_WRITE_DATA: {
            uint64_t gaddr =
                ((uint64_t)gfx_fetch(adev, rptr, 3) << 32) |
                 gfx_fetch(adev, rptr, 2);
            uint32_t val = gfx_fetch(adev, rptr, 4);

            if (gfx_addr_in_vram(adev, gaddr)) {
                *(volatile uint32_t *)(uintptr_t)
                    (adev->vram_virt + (gaddr - adev->vram_phys)) = val;
            } else if (!warned_bad_addr) {
                warned_bad_addr = true;
                adev_log("WARN", COLOR_BROWN,
                         "WRITE_DATA fora da VRAM descartado");
            }
            rptr = (rptr + 5) & RING_MASK;
            break;
        }

        case AMDGPU_GFX_OPCODE_FENCE: {
            uint64_t key =
                ((uint64_t)gfx_fetch(adev, rptr, 2) << 32) |
                 gfx_fetch(adev, rptr, 1);
            gfx_signal_key(key);
            rptr = (rptr + 3) & RING_MASK;
            break;
        }

        default:
            adev_log("WARN", COLOR_BROWN,
                     "opcode desconhecido 0x%x no GFX ring", opcode);
            rptr = (rptr + 1 + count) & RING_MASK;
            break;
        }

        __asm__ volatile("pause");
    }

    /* publica o RPTR final no registrador CP */
    amdgpu_wreg(adev, mmGFX_RB_RPTR, rptr);
}

/* ------------------------------------------------------------------ */
/* Submissão direta (helper p/ pattern/testes)                         */
/* ------------------------------------------------------------------ */

int amdgpu_ring_submit_write_fb(struct amdgpu_device *adev,
                                uint64_t fb_off_bytes, uint32_t value)
{
    unsigned long flags;
    uint64_t gaddr;

    if (!adev || !adev->initialized || !adev->fb_enabled)
        return -EINVAL;

    gaddr = adev->vram_phys + adev->fb_vram_off + fb_off_bytes;

    spin_lock_irqsave(&adev->ring_lock, &flags);
    ring_put_dw(adev, AMDGPU_PKT_HDR(3, AMDGPU_GFX_OPCODE_WRITE_DATA));
    ring_put_dw(adev, 0);   /* cntl */
    ring_put_dw(adev, (uint32_t)(gaddr & 0xFFFFFFFFu));
    ring_put_dw(adev, (uint32_t)(gaddr >> 32));
    ring_put_dw(adev, value);
    amdgpu_wreg(adev, mmGFX_RB_WPTR, adev->ring_wptr);
    spin_unlock_irqrestore(&adev->ring_lock, flags);

    /* Emulação: consome imediatamente (HW real: CP+IRQ fazem isso) */
    amdgpu_gfx_drain(adev);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Integração com o DRM Scheduler                                      */
/* ------------------------------------------------------------------ */

struct gfx_job_priv {
    uint32_t color;
    uint32_t row;       /* linha do FB que o job pinta */
};

static struct dma_fence *gfx_run_job(struct drm_sched_job *sjob)
{
    struct amdgpu_device *adev =
        container_of(sjob->entity->sched, struct amdgpu_device, sched);
    struct gfx_job_priv *priv = sjob->priv;
    struct dma_fence *hw;
    spinlock_irq_t tmp;
    unsigned long flags;
    uint64_t gaddr, key;

    hw = kmalloc(sizeof(*hw));
    if (!hw)
        return NULL;
    memset(&tmp, 0, sizeof(tmp));
    dma_fence_init(hw, &dma_fence_default_ops, &tmp,
                   adev->sched.context, sjob->fence->seqno);

    key = sjob->fence->seqno & 0xFFFFFFFFu;
    if (gfx_pend_count < GFX_PENDING_MAX) {
        gfx_pend[gfx_pend_count].key = key;
        gfx_pend[gfx_pend_count].fence = dma_fence_get(hw);
        gfx_pend_count++;
    }

    gaddr = adev->vram_phys + adev->fb_vram_off +
            (uint64_t)priv->row * adev->pitch;

    spin_lock_irqsave(&adev->ring_lock, &flags);
    ring_put_dw(adev, AMDGPU_PKT_HDR(3, AMDGPU_GFX_OPCODE_WRITE_DATA));
    ring_put_dw(adev, 0);
    ring_put_dw(adev, (uint32_t)(gaddr & 0xFFFFFFFFu));
    ring_put_dw(adev, (uint32_t)(gaddr >> 32));
    ring_put_dw(adev, priv->color);

    ring_put_dw(adev, AMDGPU_PKT_HDR(1, AMDGPU_GFX_OPCODE_FENCE));
    ring_put_dw(adev, (uint32_t)key);
    ring_put_dw(adev, (uint32_t)(key >> 32));

    amdgpu_wreg(adev, mmGFX_RB_WPTR, adev->ring_wptr);
    spin_unlock_irqrestore(&adev->ring_lock, flags);

    /* Emulação: pump síncrono consome e sinaliza a fence */
    amdgpu_gfx_drain(adev);
    return hw;
}

static void gfx_timedout_job(struct drm_sched_job *sjob)
{
    (void)sjob;
    adev_log("ERROR", COLOR_LIGHT_RED,
             "job do GFX ring estourou timeout — recovery");
}

/* ------------------------------------------------------------------ */
/* Selftest Dev 4: jobs no ring pintando linhas do FB                  */
/* ------------------------------------------------------------------ */

int amdgpu_gfx_selftest(void)
{
    static struct drm_sched_job jobs[8];
    static struct gfx_job_priv privs[8];
    const uint32_t colors[8] = {
        0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFF00,
        0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF, 0xFF808080,
    };
    int rc = -EINVAL;

    struct amdgpu_device *adev = amdgpu_adev;
    if (!adev || !adev->initialized)
        return -EINVAL;

    serial_print("[amdgpu] gfx selftest: 8 jobs no GFX ring\n");

    for (int i = 0; i < 8; i++) {
        privs[i].color = colors[i];
        privs[i].row = (uint32_t)i;
        rc = drm_sched_job_init(&jobs[i], &adev->entity, &privs[i]);
        if (rc) {
            adev_log("FAIL", COLOR_LIGHT_RED, "job_init %d falhou", i);
            goto out;
        }
        drm_sched_entity_push_job(&jobs[i], &adev->entity);
    }

    drm_sched_flush(&adev->sched);

    bool ok = true;
    for (int i = 0; i < 8; i++) {
        uint32_t got = *(volatile uint32_t *)(uintptr_t)
            ((uintptr_t)adev->fb_vaddr + (uintptr_t)i * adev->pitch);

        if (got != colors[i] || jobs[i].state != DRM_SCHED_JOB_DONE ||
            !dma_fence_is_signaled(jobs[i].fence)) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        adev_log("FAIL", COLOR_LIGHT_RED,
                 "pixels/fences divergentes após drain do ring");
        rc = -EINVAL;
        goto out;
    }

    /* limpa as linhas usadas (o pattern assume fundo escuro) */
    for (int i = 0; i < 8; i++)
        *(volatile uint32_t *)(uintptr_t)
            ((uintptr_t)adev->fb_vaddr + (uintptr_t)i * adev->pitch) = 0;

    adev_log("PASS", COLOR_LIGHT_GREEN,
             "gfx selftest OK (8 jobs WRITE_DATA/FENCE no ring)");
    rc = 0;

out:
    return rc;
}
