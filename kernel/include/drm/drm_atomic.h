/*
 * drm/drm_atomic.h — Atomic KMS base nativa.
 *
 * Fase 2 Dev 3 — entrega: commit atômico de CRTC+connector com rollback
 * em caso de validação falhar (teste unitário roda no boot).
 *
 * Modelo (subset do Linux atomic modeset):
 *   - Um drm_atomic_kms é o mode_config de um drm_device: tabelas de
 *     crtc / encoder / connector / plane com estado corrente instalado.
 *   - drm_atomic_state_get() faz snapshot (old=new=current).
 *   - Setters mutam SOMENTE `new`.
 *   - drm_atomic_commit() valida `new` inteiro e instala tudo ou nada:
 *     validação falhou => nenhum campo é tocado (rollback implícito,
 *     estado corrente permanece o antigo).
 *   - Propriedades nomeadas ficam registradas por objeto para
 *     introspecção (mesma idea de properties do atomic/KMS).
 */

#ifndef _DRM_ATOMIC_H_
#define _DRM_ATOMIC_H_

#include <stdint.h>
#include <stdbool.h>
#include <spinlock.h>

struct drm_device;

#define DRM_ATOMIC_MAX_CRTCS      4
#define DRM_ATOMIC_MAX_ENCODERS   4
#define DRM_ATOMIC_MAX_CONNECTORS 4
#define DRM_ATOMIC_MAX_PLANES     8
#define DRM_ATOMIC_MAX_PROPS      8
#define DRM_ATOMIC_PROP_NAME_LEN  24

#define DRM_ATOMIC_NO_LINK        0xFFFFFFFFu

/* ---- Objetos KMS (estado instalado) ---- */

struct drm_atomic_crtc {
    bool     used;
    uint32_t id;
    bool     active;
    uint32_t fb_handle;
    uint32_t hdisplay, vdisplay, vrefresh;
    uint32_t encoder_id;
};

struct drm_atomic_encoder {
    bool     used;
    uint32_t id;
    uint32_t crtc_id;
    uint32_t possible_crtcs;   /* bitmask de SLOT de crtc */
};

struct drm_atomic_connector {
    bool     used;
    uint32_t id;
    uint32_t encoder_id;
    bool     connected;
};

struct drm_atomic_plane {
    bool     used;
    uint32_t id;
    uint32_t crtc_id;
    uint32_t fb_handle;
    uint32_t x, y, width, height;
};

/* Propriedade nomeada (introspecção estilo atomic props) */
struct drm_atomic_property {
    char     name[DRM_ATOMIC_PROP_NAME_LEN];
    uint64_t value;
};

/* Tabelas completas de um snapshot (old ou new) */
struct drm_atomic_snapshot {
    struct drm_atomic_crtc      crtcs[DRM_ATOMIC_MAX_CRTCS];
    struct drm_atomic_encoder   encoders[DRM_ATOMIC_MAX_ENCODERS];
    struct drm_atomic_connector connectors[DRM_ATOMIC_MAX_CONNECTORS];
    struct drm_atomic_plane     planes[DRM_ATOMIC_MAX_PLANES];
};

/* Hooks opcionais do driver (Fase 4 liga o DC aqui) */
struct drm_atomic_funcs {
    /* Chamado após instalação bem-sucedida do novo estado. */
    int (*commit)(struct drm_device *dev);
};

/* Mode config por device */
struct drm_atomic_kms {
    struct drm_device              *dev;
    spinlock_t                      lock;
    uint32_t                        max_crtc, max_encoder, max_conn, max_plane;
    struct drm_atomic_snapshot      current;
    struct drm_atomic_property      crtc_props[DRM_ATOMIC_MAX_CRTCS][DRM_ATOMIC_MAX_PROPS];
    struct drm_atomic_funcs const  *funcs;
};

/* Estado atômico em construção */
struct drm_atomic_state {
    struct drm_atomic_kms       *kms;
    struct drm_atomic_snapshot   old;
    struct drm_atomic_snapshot   new;
    bool                         committed;
};

/* ---- Registro do mode config ---- */
int  drm_atomic_kms_register(struct drm_device *dev,
                             uint32_t max_crtc, uint32_t max_encoder,
                             uint32_t max_conn, uint32_t max_plane,
                             const struct drm_atomic_funcs *funcs);
void drm_atomic_kms_unregister(struct drm_device *dev);
struct drm_atomic_kms *drm_atomic_get_kms(struct drm_device *dev);

/* Readback do estado corrente (pode retornar NULL) */
const struct drm_atomic_crtc      *drm_atomic_kms_crtc(struct drm_atomic_kms *kms, uint32_t id);
const struct drm_atomic_encoder   *drm_atomic_kms_encoder(struct drm_atomic_kms *kms, uint32_t id);
const struct drm_atomic_connector *drm_atomic_kms_connector(struct drm_atomic_kms *kms, uint32_t id);
const struct drm_atomic_plane     *drm_atomic_kms_plane(struct drm_atomic_kms *kms, uint32_t id);

/* ---- Estado atômico ---- */
struct drm_atomic_state *drm_atomic_state_get(struct drm_device *dev);
void drm_atomic_state_put(struct drm_atomic_state *state);

/* Setters (mutam apenas new). Retornam 0 ou -EINVAL. */
int drm_atomic_crtc_set_active(struct drm_atomic_state *st, uint32_t crtc_id, bool active);
int drm_atomic_crtc_set_mode(struct drm_atomic_state *st, uint32_t crtc_id,
                             uint32_t hdisplay, uint32_t vdisplay, uint32_t vrefresh);
int drm_atomic_crtc_set_fb(struct drm_atomic_state *st, uint32_t crtc_id,
                           uint32_t fb_handle, uint32_t width, uint32_t height,
                           uint32_t pitch);
int drm_atomic_encoder_assign_crtc(struct drm_atomic_state *st, uint32_t enc_id,
                                   uint32_t crtc_id /* ou DRM_ATOMIC_NO_LINK */);
int drm_atomic_encoder_set_possible_crtcs(struct drm_atomic_state *st,
                                          uint32_t enc_id, uint32_t mask);
int drm_atomic_connector_set_encoder(struct drm_atomic_state *st, uint32_t conn_id,
                                     uint32_t enc_id /* ou DRM_ATOMIC_NO_LINK */);
int drm_atomic_plane_set_fb(struct drm_atomic_state *st, uint32_t plane_id,
                            uint32_t crtc_id, uint32_t fb_handle,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Propriedades nomeadas (introspecção) */
int drm_atomic_set_property(struct drm_atomic_state *st, uint32_t obj_id,
                            const char *name, uint64_t value);
uint64_t drm_atomic_get_property(struct drm_atomic_state *st, uint32_t obj_id,
                                 const char *name, bool *found);

/* ---- Validação / commit ---- */
bool drm_atomic_check_only(struct drm_atomic_state *st, const char **reason);
int  drm_atomic_commit(struct drm_atomic_state *st, const char **reason);

/* ---- Selftest de boot ---- */
int drm_atomic_test(void);

#endif /* _DRM_ATOMIC_H_ */
