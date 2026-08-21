/*
 * dc_core.c — núcleo do Display Core nativo (Fase 4).
 *
 * Responsabilidades (espelhando o papel do dal core do DC real):
 *   - ciclo de vida do dc_context (links, pool por ASIC)
 *   - commit atômico de streams (blank → program → unblank; falha
 *     preserva o estado anterior)
 *   - flip tear-free (troca só o endereço GRPH)
 *   - HPD: leitura bruta + poll no idle com eventos connect/disconnect
 *   - gerenciador MST (payload table) com selftest puro
 *   - modestest interno: troca resolução pelo caminho DC completo
 */

#include <amdgpu_dc.h>
#include <amdgpu.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>
#include <screen.h>
#include <timer.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

void adev_log(const char *tag, vga_color_t color, const char *fmt, ...);

const struct dc_resource_pool *dc_dce_pool_for(enum amdgpu_asic_type t);
const struct dc_resource_pool *dc_dcn_pool_for(enum amdgpu_asic_type t);

const struct dc_resource_pool *dc_pool_for_asic(enum amdgpu_asic_type t)
{
    const struct dc_resource_pool *p = dc_dcn_pool_for(t);
    return p ? p : dc_dce_pool_for(t);
}

/* ------------------------------------------------------------------ */
/* Ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

int dc_create(struct amdgpu_device *adev, struct dc_context **out)
{
    struct dc_context *ctx;
    const struct dc_resource_pool *pool;
    int i;

    if (!adev || !out)
        return -EINVAL;

    pool = dc_pool_for_asic(adev->asic_type);
    if (!pool) {
        adev_log("WARN", COLOR_BROWN,
                 "DC: sem resource pool para %s", adev->asic_name);
        return -EINVAL;
    }

    ctx = kmalloc(sizeof(*ctx));
    if (!ctx)
        return -ENOMEM;
    memset(ctx, 0, sizeof(*ctx));

    ctx->adev = adev;
    ctx->pool = pool;

    for (i = 0; i < DC_MAX_LINKS; i++)
        ctx->links[i].link_id = i;

    /* estado inicial do HPD lido direto do HW */
    ctx->links[0].connected = dc_hpd_read_raw(ctx);

    ctx->ready = true;
    adev_log("INFO", COLOR_LIGHT_CYAN, "DC: %s pronto (%u crtc, HPD=%s)",
             pool->name, pool->num_crtc,
             ctx->links[0].connected ? "on" : "off");

    *out = ctx;
    return 0;
}

void dc_destroy(struct dc_context **ctx_p)
{
    if (!ctx_p || !*ctx_p)
        return;
    kfree(*ctx_p);
    *ctx_p = NULL;
}

/* ------------------------------------------------------------------ */
/* Commit atômico                                                      */
/* ------------------------------------------------------------------ */

int dc_commit_streams(struct dc_context *ctx, struct dc_stream_state *s)
{
    int rc;

    if (!ctx || !ctx->ready || !s || !s->link)
        return -EINVAL;

    /* validação: link precisa estar conectado (HPD) */
    if (!s->link->connected) {
        adev_log("INFO", COLOR_BROWN,
                 "DC: commit rejeitado — link%u desconectado (HPD)",
                 s->link->link_id);
        return -ENOTCONN;
    }
    if (!s->hdisplay || !s->vdisplay || !s->vrefresh ||
        !s->plane.width || !s->plane.height)
        return -EINVAL;

    rc = ctx->pool->apply_stream(ctx->adev, s);
    if (rc)
        return rc;   /* HW recusou: stream anterior permanece ativo */

    if (ctx->current && ctx->current != s)
        ctx->current->enabled = false;
    s->enabled = true;
    ctx->current = s;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Flip tear-free                                                      */
/* ------------------------------------------------------------------ */

int dc_flip(struct dc_context *ctx, uint64_t surface_vram_off)
{
    struct amdgpu_device *adev;

    if (!ctx || !ctx->ready || !ctx->current || !ctx->current->enabled)
        return -EINVAL;
    adev = ctx->adev;

    /* Troca apenas o endereço GRPH — o CRTC busca o novo frame no
     * próximo vsync da emulação (sem blanking => sem tearing). */
    amdgpu_wreg(adev, mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS,
                (uint32_t)(surface_vram_off & 0xFFFFFFFFu));
    amdgpu_wreg(adev, mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH,
                (uint32_t)(surface_vram_off >> 32));
    return 0;
}

/* ------------------------------------------------------------------ */
/* HPD                                                                 */
/* ------------------------------------------------------------------ */

bool dc_hpd_read_raw(struct dc_context *ctx)
{
    if (!ctx || !ctx->adev)
        return false;
    return !!(amdgpu_rreg(ctx->adev, mmHPD0_STATUS) & 1u);
}

void dc_hpd_poll(struct dc_context *ctx)
{
    bool now;

    if (!ctx || !ctx->ready)
        return;

    now = dc_hpd_read_raw(ctx);
    if (now == ctx->links[0].connected)
        return;

    ctx->links[0].connected = now;

    if (now) {
        ctx->hpd_connect_events++;
        adev_log("OK", COLOR_LIGHT_GREEN,
                 "DC: hotplug CONNECT no link0");
        /* re-aplica o stream que estava ativo antes do disconnect */
        if (ctx->current && !ctx->current->enabled) {
            if (ctx->pool->apply_stream(ctx->adev, ctx->current) == 0)
                ctx->current->enabled = true;
        }
    } else {
        ctx->hpd_disconnect_events++;
        adev_log("WARN", COLOR_BROWN,
                 "DC: hotplug DISCONNECT no link0 — blanking stream");
        if (ctx->current) {
            ctx->pool->set_power(ctx->adev, false);
            ctx->current->enabled = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* MST framework (payload table)                                       */
/* ------------------------------------------------------------------ */

int dc_mst_alloc_payload(struct mst_topology *mst, uint32_t slots)
{
    if (!mst || !mst->enabled || slots == 0)
        return -EINVAL;
    if (mst->num_payloads >= DC_MST_MAX_PAYLOADS)
        return -ENOMEM;

    for (uint32_t i = 0; i < mst->num_payloads; i++) {
        if (mst->payloads[i] == slots)   /* já existe: idempotente */
            return (int)i;
    }
    mst->payloads[mst->num_payloads++] = slots;
    return (int)(mst->num_payloads - 1);
}

void dc_mst_free_payload(struct mst_topology *mst, uint32_t index)
{
    if (!mst || index >= mst->num_payloads)
        return;
    for (uint32_t i = index; i + 1 < mst->num_payloads; i++)
        mst->payloads[i] = mst->payloads[i + 1];
    mst->num_payloads--;
}

static bool dc_mst_selftest_inner(void)
{
    struct mst_topology mst;
    int a, b;

    memset(&mst, 0, sizeof(mst));
    mst.enabled = true;

    a = dc_mst_alloc_payload(&mst, 64);      /* monitor A: 64 slots */
    b = dc_mst_alloc_payload(&mst, 32);      /* monitor B: 32 slots */
    if (a != 0 || b != 1 || mst.num_payloads != 2)
        return false;
    if (dc_mst_alloc_payload(&mst, 64) != 0) /* idempotência */
        return false;

    dc_mst_free_payload(&mst, 0);            /* remove A; B desloca */
    if (mst.num_payloads != 1 || mst.payloads[0] != 32)
        return false;
    if (dc_mst_free_payload(&mst, 5), mst.num_payloads != 1)
        return false;                        /* índice inválido: no-op */

    mst.enabled = false;
    if (dc_mst_alloc_payload(&mst, 16) != -EINVAL)
        return false;                        /* topologia inativa */
    return true;
}

/* ------------------------------------------------------------------ */
/* Selftest Fase 4 (+ modestest interno)                               */
/* ------------------------------------------------------------------ */

static struct dc_stream_state test_stream_a;   /* 1920x1080@60 */
static struct dc_stream_state test_stream_b;   /* 1280x720@60  */

int amdgpu_dc_selftest(void)
{
    struct amdgpu_device *adev = amdgpu_adev;
    int failures = 0;

    if (!adev || !adev->dc_ctx || !adev->dc_ctx->ready)
        return -EINVAL;

    serial_print("[amdgpu-dc] selftest Fase 4 (MST + HPD + modetest)\n");

    /* ---- MST payload manager ---- */
    if (dc_mst_selftest_inner()) {
        adev_log("PASS", COLOR_LIGHT_GREEN,
                 "DC MST: alloc/free/idempotencia OK");
    } else {
        failures++;
        adev_log("FAIL", COLOR_LIGHT_RED, "DC MST: payload manager");
    }

    /* ---- Commit rejeitado sem link (HPD off simulado na estrutura) --
     * usa um stream com link "fantasma" desconectado */
    {
        struct dc_link dead_link = { .link_id = 3, .connected = false };
        struct dc_stream_state ghost = { 0 };

        ghost.hdisplay = 640; ghost.vdisplay = 480; ghost.vrefresh = 60;
        ghost.link = &dead_link;
        if (dc_commit_streams(adev->dc_ctx, &ghost) == -ENOTCONN) {
            adev_log("PASS", COLOR_LIGHT_GREEN,
                     "DC commit: ENOTCONN sem HPD OK");
        } else {
            failures++;
            adev_log("FAIL", COLOR_LIGHT_RED,
                     "DC commit: deveria falhar sem link");
        }
    }

    /* ---- Modestest interno: 1080p -> 720p -> 1080p via caminho DC ---- */
    {
        struct dc_context *ctx = adev->dc_ctx;
        struct dc_stream_state *cur = ctx->current;

        if (!cur) {
            failures++;
            adev_log("FAIL", COLOR_LIGHT_RED,
                     "modestest: nenhum stream ativo");
        } else {
            /* stream B: 1280x720@60 reaproveitando a MESMA superfície
             * (o FB é maior que o modo — pitch continua válido para a
             * verificação de flip; o scanout só enxerga 720 linhas) */
            test_stream_b = *cur;
            test_stream_b.hdisplay = 1280;
            test_stream_b.vdisplay = 720;
            test_stream_b.vrefresh = 60;
            test_stream_b.plane.width = 1280;
            test_stream_b.plane.height = 720;
            test_stream_b.plane.surface_vram_off =
                cur->plane.surface_vram_off;
            test_stream_b.link = cur->link;

            if (dc_commit_streams(ctx, &test_stream_b)) {
                failures++;
                adev_log("FAIL", COLOR_LIGHT_RED,
                         "modestest: commit 1280x720@60 falhou");
            } else if (dc_flip(ctx, test_stream_b.plane.surface_vram_off)) {
                failures++;
                adev_log("FAIL", COLOR_LIGHT_RED,
                         "modestest: flip em 720p falhou");
            } else {
                adev_log("PASS", COLOR_LIGHT_GREEN,
                         "modestest: 1280x720@60 aplicado + flip OK");
            }

            /* volta pro 1080p original */
            test_stream_a = *cur;
            if (dc_commit_streams(ctx, &test_stream_a)) {
                failures++;
                adev_log("FAIL", COLOR_LIGHT_RED,
                         "modestest: retorno a 1080p falhou");
            } else {
                uint32_t ctl = amdgpu_rreg(adev, mmCRTC0_CONTROL);
                uint32_t xend = amdgpu_rreg(
                    adev, mmCRTC0_GRPH_X_END);
                if (ctl == AMDGPU_CRTC_ENABLE &&
                    xend == adev->mode_w) {
                    adev_log("PASS", COLOR_LIGHT_GREEN,
                            "modestest: retorno 1920x1080@60 "
                            "(readback X_END=%u)", xend);
                } else {
                    failures++;
                    adev_log("FAIL", COLOR_LIGHT_RED,
                             "modestest: readback pos-retorno divergente");
                }
            }
        }
    }

    if (failures == 0) {
        adev_log("PASS", COLOR_LIGHT_GREEN,
                 "amdgpu DC: ALL CHECKS PASSED");
        return 0;
    }
    adev_log("FAIL", COLOR_LIGHT_RED,
             "amdgpu DC: %d check(s) FAILED", failures);
    return -EINVAL;
}
