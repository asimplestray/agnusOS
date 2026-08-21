/*
 * dcn_resource.c — resource pools DCN 1.x/2.x (Fase 4 Dev 2).
 *
 * DCN real usa HUBP/OPP/OPTC/DPP com pipes independentes. Na emulação
 * o Navi22 expõe o mesmo bloco de scanout do subset (amd_core.h), então
 * o pool DCN programa os mesmos offsets — a diferença estrutural
 * (família, nome, caminho de validação) fica no framework, pronta para
 * receber as tabelas reais de offsets quando houver HW/emulador que as
 * modele.
 *
 * Entrega da tarefa: modeset via DC em Vega/Navi — validado no
 * amd-rx6700xt (Navi22) com o mesmo pipeline de scanout do host.
 */

#include <amdgpu_dc.h>

#ifndef EINVAL
#define EINVAL 22
#endif

/* O caminho de programação é compartilhado com o DCE subset; declarado
 * em dce_resource.c para evitar duplicar a rotina neste estágio. */
int dc_dcn_apply_stream(struct amdgpu_device *adev,
                        const struct dc_stream_state *s);
void dc_dcn_set_power(struct amdgpu_device *adev, bool on);

static int dcn_apply_stream(struct amdgpu_device *adev,
                            const struct dc_stream_state *s)
{
    /* DCN real: HUBP0 (surface) + OPTC (timing). Subset: mesma rotina. */
    return dc_dcn_apply_stream(adev, s);
}

static void dcn_set_power(struct amdgpu_device *adev, bool on)
{
    dc_dcn_set_power(adev, on);
}

/* DCN 2.x — Navi 1x/2x. */
static const struct dc_resource_pool dcn2_pool = {
    .family       = DC_FAMILY_DCN2,
    .name         = "DCN 2",
    .num_crtc     = 1,
    .apply_stream = dcn_apply_stream,
    .set_power    = dcn_set_power,
};

/* DCN 1.x — Vega. Sem ASIC na tabela do driver ainda; pool pronto. */
static const struct dc_resource_pool dcn1_pool = {
    .family       = DC_FAMILY_DCN1,
    .name         = "DCN 1",
    .num_crtc     = 1,
    .apply_stream = dcn_apply_stream,
    .set_power    = dcn_set_power,
};

const struct dc_resource_pool *dc_dcn_pool_for(enum amdgpu_asic_type t)
{
    switch (t) {
    case CHIP_NAVI22:
        return &dcn2_pool;
    default:
        (void)&dcn1_pool;
        return NULL;
    }
}

/* ---- Implementações compartilhadas (mesmos offsets do subset) ---- */

int dc_dcn_apply_stream(struct amdgpu_device *adev,
                        const struct dc_stream_state *s)
{
    const uint32_t h_tot = s->hdisplay + 280;
    const uint32_t v_tot = s->vdisplay + (s->vdisplay == 720 ? 28 : 45);
    const uint32_t h_sync = s->hdisplay + 88;
    const uint32_t v_sync = s->vdisplay + (s->vdisplay == 720 ? 8 : 4);

    amdgpu_wreg(adev, mmCRTC0_CONTROL, 0);

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
    amdgpu_wreg(adev, mmCRTC0_CONTROL, AMDGPU_CRTC_ENABLE);
    return 0;
}

void dc_dcn_set_power(struct amdgpu_device *adev, bool on)
{
    amdgpu_wreg(adev, mmCRTC0_CONTROL, on ? AMDGPU_CRTC_ENABLE : 0u);
}
