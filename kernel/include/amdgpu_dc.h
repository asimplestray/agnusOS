/*
 * amdgpu_dc.h — Display Core nativo (Fase 4) para o amdgpu do AgnusOS.
 *
 * Porta a ESTRUTURA do Linux DC (dal core / resource / stream / link)
 * sobre o subset de display da emulação vgpu. Sem Audio/PSR/MST real:
 *
 *   Dev 1 — resource pools DCE 8/10/11 (dce_resource.c)  ✅
 *   Dev 2 — pools DCN 1.x/2.x (dcn_resource.c; Navi22 na vgpu) ✅
 *   Dev 3 — dc_link com HPD (reg da emulação, toggleável via QOM
 *           property `hpd_connected`) + gerenciador MST (framework +
 *           selftests puros; sem hub AUX real na emulação)      ✅
 *   Dev 4 — integração com atomic KMS (drm_atomic_funcs.commit →
 *           dc_commit), flip double-buffer sem tearing e fallback
 *           fbdev direto caso o DC não suba                      ✅
 */

#ifndef _AMDGPU_DC_H_
#define _AMDGPU_DC_H_

#include <stdint.h>
#include <stdbool.h>
#include <amdgpu.h>

#define DC_MAX_LINKS        4
#define DC_MST_MAX_PAYLOADS 8

enum dc_family {
    DC_FAMILY_NONE = 0,
    DC_FAMILY_DCE8,     /* GCN1-2 (Sea/Land)            */
    DC_FAMILY_DCE10,    /* GCN3 (Volcanic/Tropical)     */
    DC_FAMILY_DCE11,    /* GCN4 VI (Polaris)            */
    DC_FAMILY_DCN1,     /* Vega (DCN 1.x)               */
    DC_FAMILY_DCN2,     /* Navi 1x/2x (DCN 2.x)         */
};

/* ---- Resource pool (ops por família — resource_pool do DC real) ---- */
struct dc_stream_state;
struct dc_context;

struct dc_resource_pool {
    enum dc_family family;
    const char    *name;
    unsigned       num_crtc;

    /* Programação completa do stream no HW (timing+GRPH+enable). */
    int (*apply_stream)(struct amdgpu_device *adev,
                        const struct dc_stream_state *stream);

    /* Liga/desliga o CRTC (blanking). */
    void (*set_power)(struct amdgpu_device *adev, bool on);
};

const struct dc_resource_pool *dc_pool_for_asic(enum amdgpu_asic_type t);

/* ---- Link (connector + HPD + MST) ---- */
struct mst_topology {
    bool     enabled;
    uint32_t payloads[DC_MST_MAX_PAYLOADS];   /* VC payload table */
    uint32_t num_payloads;
};

struct dc_link {
    uint32_t link_id;
    bool     connected;       /* espelha HPD do HW/emulador */
};

/* ---- Stream / plane ---- */
struct dc_plane_state {
    uint64_t surface_vram_off;   /* offset do FB dentro do BAR1 */
    uint32_t width, height;
    uint32_t format;             /* AMDGPU_GRPH_FORMAT_* */
};

struct dc_stream_state {
    uint32_t hdisplay, vdisplay, vrefresh;
    struct dc_plane_state plane;
    struct dc_link *link;
    bool     enabled;
};

/* ---- Contexto do DC ---- */
struct dc_context {
    struct amdgpu_device          *adev;
    const struct dc_resource_pool *pool;
    struct dc_link                 links[DC_MAX_LINKS];
    struct dc_stream_state        *current;   /* single-head (v0.x) */
    bool                           ready;

    /* eventos de HPD contados p/ o log/idle tick */
    uint32_t hpd_connect_events;
    uint32_t hpd_disconnect_events;
};

/* ---- Ciclo de vida ---- */
int  dc_create(struct amdgpu_device *adev, struct dc_context **out);
void dc_destroy(struct dc_context **ctx);

/* ---- Commit ----
 * Valida o stream contra o link (HPD), blanka, programa via pool,
 * unblanka. Atômico: falha => estado anterior intacto. */
int  dc_commit_streams(struct dc_context *ctx,
                       struct dc_stream_state *stream);

/* Flip tear-free: troca apenas o endereço da superfície GRPH. */
int  dc_flip(struct dc_context *ctx, uint64_t surface_vram_off);

/* ---- HPD ---- */
bool dc_hpd_read_raw(struct dc_context *ctx);   /* bit bruto do reg  */
void dc_hpd_poll(struct dc_context *ctx);       /* chamado no idle   */

/* ---- MST framework (payload table; selftest puro no boot) ---- */
int  dc_mst_alloc_payload(struct mst_topology *mst, uint32_t slots);
void dc_mst_free_payload(struct mst_topology *mst, uint32_t index);
int  dc_mst_selftest(void);

/* ---- Selftest Fase 4 ----
 * Inclui o "modestest interno": troca resolução pelo caminho DC
 * completo (1920x1080@60 -> 1280x720@60 -> volta) validando commit,
 * flip e rollback. */
int amdgpu_dc_selftest(void);

#endif /* _AMDGPU_DC_H_ */
