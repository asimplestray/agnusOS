/*
 * dce_resource.c — resource pools DCE 8/10/11 (Fase 4 Dev 1).
 *
 * Cada família DCE tem seu resource_pool com a rotina apply_stream.
 * Os offsets abaixo seguem o subset implementado pela emulação vgpu
 * (ver amd_core.h do fork): bloco CRTC0/GRPH único, igual para as
 * famílias no emulador — em hardware real cada pool carregaria suas
 * tabelas de offsets próprias (dce80/dce100/dce110).
 *
 * A programação segue a ordem clássica do DC real:
 *   blank → timing → GRPH surface/pitch/format → HDP flush → unblank
 */

#include <amdgpu_dc.h>
#include <string.h>

#ifndef EINVAL
#define EINVAL 22
#endif

static void dce_set_power(struct amdgpu_device *adev, bool on)
{
    amdgpu_wreg(adev, mmCRTC0_CONTROL, on ? AMDGPU_CRTC_ENABLE : 0u);
}

static int dce_apply_stream(struct amdgpu_device *adev,
                            const struct dc_stream_state *s)
{
    const uint32_t h_tot  = s->hdisplay + 280;   /* ~DMT: front/back/sync */
    const uint32_t v_tot  = s->vdisplay + (s->vdisplay == 720 ? 28 : 45);
    const uint32_t h_sync = s->hdisplay + 88;
    const uint32_t v_sync = s->vdisplay + (s->vdisplay == 720 ? 8 : 4);

    /* blank */
    amdgpu_wreg(adev, mmCRTC0_CONTROL, 0);

    /* timing */
    amdgpu_wreg(adev, mmCRTC0_H_TOTAL,
                ((h_tot - 1) << 16) | (s->hdisplay - 1));
    amdgpu_wreg(adev, mmCRTC0_H_BLANK_START_END,
                ((h_tot - 1) << 16) | (s->hdisplay - 1));
    amdgpu_wreg(adev, mmCRTC0_H_SYNC_A,
                ((h_sync + 48 - 1) << 16) | (h_sync - 1));
    amdgpu_wreg(adev, mmCRTC0_V_TOTAL,
                ((v_tot - 1) << 16) | (s->vdisplay - 1));
    amdgpu_wreg(adev, mmCRTC0_V_BLANK_START_END,
                ((v_tot - 1) << 16) | (s->vdisplay - 1));
    amdgpu_wreg(adev, mmCRTC0_V_SYNC_A,
                ((v_sync + 5 - 1) << 16) | (v_sync - 1));

    /* superfície GRPH */
    amdgpu_wreg(adev, mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS,
                (uint32_t)(s->plane.surface_vram_off & 0xFFFFFFFFu));
    amdgpu_wreg(adev, mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH,
                (uint32_t)(s->plane.surface_vram_off >> 32));
    amdgpu_wreg(adev, mmCRTC0_GRPH_PITCH, s->plane.width * 4);
    amdgpu_wreg(adev, mmCRTC0_GRPH_X_START, 0);
    amdgpu_wreg(adev, mmCRTC0_GRPH_Y_START, 0);
    amdgpu_wreg(adev, mmCRTC0_GRPH_X_END, s->plane.width);
    amdgpu_wreg(adev, mmCRTC0_GRPH_Y_END, s->plane.height);
    amdgpu_wreg(adev, mmCRTC0_GRPH_FORMAT, AMDGPU_GRPH_FORMAT_32BPP);

    amdgpu_wreg(adev, mmHDP_HOST_PATH_CNTL, 1);
    amdgpu_wreg(adev, mmHDP_FLUSH_INVALIDATE, 1);

    /* unblank */
    amdgpu_wreg(adev, mmCRTC0_CONTROL, AMDGPU_CRTC_ENABLE);
    return 0;
}

/* DCE 10 — GCN3 (Volcanic/Tropical). Subset compartilhado na emulação. */
static const struct dc_resource_pool dce10_pool = {
    .family       = DC_FAMILY_DCE10,
    .name         = "DCE 10",
    .num_crtc     = 1,
    .apply_stream = dce_apply_stream,
    .set_power    = dce_set_power,
};

/* DCE 11 — Polaris (VI). No HW real: tabelas dce110. */
static const struct dc_resource_pool dce11_pool = {
    .family       = DC_FAMILY_DCE11,
    .name         = "DCE 11",
    .num_crtc     = 1,      /* subset da emulação: CRTC0 */
    .apply_stream = dce_apply_stream,
    .set_power    = dce_set_power,
};

const struct dc_resource_pool *dc_dce_pool_for(enum amdgpu_asic_type t)
{
    switch (t) {
    case CHIP_POLARIS10:
    case CHIP_POLARIS11:
        return &dce11_pool;
    case CHIP_POLARIS12:
        return &dce10_pool;
    default:
        /* DCE 8 (HD7xxx/R9): pool pronto, sem ASIC na tabela ainda */
        return NULL;
    }
}
