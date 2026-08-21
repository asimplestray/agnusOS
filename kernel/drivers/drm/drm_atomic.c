/*
 * drm_atomic.c — Atomic KMS base nativa.
 *
 * Fase 2 Dev 3. Ver drm/drm_atomic.h para o modelo commit/rollback.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_device.h>
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

#define DRM_ATOMIC_MAX_DEVS 16

static struct {
    struct drm_atomic_kms *kms;
} atomic_devs[DRM_ATOMIC_MAX_DEVS];
static spinlock_t atomic_reg_lock = SPINLOCK_INIT;

static void atomic_log(const char *tag, vga_color_t color, const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_print("[DRM-ATOMIC] ");
    serial_print(buf);
    serial_print("\n");
    screen_log(tag, color, buf);
}

/* ------------------------------------------------------------------ */
/* Registro                                                            */
/* ------------------------------------------------------------------ */

static uint32_t next_obj_id = 1;

int drm_atomic_kms_register(struct drm_device *dev,
                            uint32_t max_crtc, uint32_t max_encoder,
                            uint32_t max_conn, uint32_t max_plane,
                            const struct drm_atomic_funcs *funcs)
{
    struct drm_atomic_kms *kms;

    if (!dev || max_crtc > DRM_ATOMIC_MAX_CRTCS ||
        max_encoder > DRM_ATOMIC_MAX_ENCODERS ||
        max_conn > DRM_ATOMIC_MAX_CONNECTORS ||
        max_plane > DRM_ATOMIC_MAX_PLANES)
        return -EINVAL;

    if (drm_atomic_get_kms(dev))
        return -EINVAL;   /* já registrado */

    kms = kmalloc(sizeof(struct drm_atomic_kms));
    if (!kms)
        return -ENOMEM;
    memset(kms, 0, sizeof(*kms));

    spinlock_init(&kms->lock);
    kms->dev = dev;
    kms->max_crtc = max_crtc;
    kms->max_encoder = max_encoder;
    kms->max_conn = max_conn;
    kms->max_plane = max_plane;
    kms->funcs = funcs;

    /* IDs únicos globais por objeto */
    for (uint32_t i = 0; i < max_crtc; i++) {
        kms->current.crtcs[i].used = true;
        kms->current.crtcs[i].id = next_obj_id++;
        kms->current.crtcs[i].encoder_id = DRM_ATOMIC_NO_LINK;
    }
    for (uint32_t i = 0; i < max_encoder; i++) {
        kms->current.encoders[i].used = true;
        kms->current.encoders[i].id = next_obj_id++;
        kms->current.encoders[i].crtc_id = DRM_ATOMIC_NO_LINK;
        /* default: todos os slots possíveis */
        kms->current.encoders[i].possible_crtcs =
            (max_crtc >= 32) ? 0xFFFFFFFFu : ((1u << max_crtc) - 1);
    }
    for (uint32_t i = 0; i < max_conn; i++) {
        kms->current.connectors[i].used = true;
        kms->current.connectors[i].id = next_obj_id++;
        kms->current.connectors[i].encoder_id = DRM_ATOMIC_NO_LINK;
        kms->current.connectors[i].connected = true;
    }
    for (uint32_t i = 0; i < max_plane; i++) {
        kms->current.planes[i].used = true;
        kms->current.planes[i].id = next_obj_id++;
        kms->current.planes[i].crtc_id = DRM_ATOMIC_NO_LINK;
    }

    spin_lock(&atomic_reg_lock);
    for (int i = 0; i < DRM_ATOMIC_MAX_DEVS; i++) {
        if (!atomic_devs[i].kms) {
            atomic_devs[i].kms = kms;
            spin_unlock(&atomic_reg_lock);
            atomic_log("INFO", COLOR_LIGHT_CYAN,
                       "mode config registrado (%u crtc/%u enc/%u conn/%u plane)",
                       max_crtc, max_encoder, max_conn, max_plane);
            return 0;
        }
    }
    spin_unlock(&atomic_reg_lock);

    kfree(kms);
    return -ENOMEM;
}

void drm_atomic_kms_unregister(struct drm_device *dev)
{
    if (!dev)
        return;

    spin_lock(&atomic_reg_lock);
    for (int i = 0; i < DRM_ATOMIC_MAX_DEVS; i++) {
        if (atomic_devs[i].kms && atomic_devs[i].kms->dev == dev) {
            kfree(atomic_devs[i].kms);
            atomic_devs[i].kms = NULL;
            break;
        }
    }
    spin_unlock(&atomic_reg_lock);
}

struct drm_atomic_kms *drm_atomic_get_kms(struct drm_device *dev)
{
    if (!dev)
        return NULL;
    for (int i = 0; i < DRM_ATOMIC_MAX_DEVS; i++) {
        if (atomic_devs[i].kms && atomic_devs[i].kms->dev == dev)
            return atomic_devs[i].kms;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Readback                                                            */
/* ------------------------------------------------------------------ */

const struct drm_atomic_crtc *drm_atomic_kms_crtc(struct drm_atomic_kms *kms,
                                                  uint32_t id)
{
    if (!kms)
        return NULL;
    for (uint32_t i = 0; i < kms->max_crtc; i++)
        if (kms->current.crtcs[i].used && kms->current.crtcs[i].id == id)
            return &kms->current.crtcs[i];
    return NULL;
}

const struct drm_atomic_encoder *drm_atomic_kms_encoder(
    struct drm_atomic_kms *kms, uint32_t id)
{
    if (!kms)
        return NULL;
    for (uint32_t i = 0; i < kms->max_encoder; i++)
        if (kms->current.encoders[i].used && kms->current.encoders[i].id == id)
            return &kms->current.encoders[i];
    return NULL;
}

const struct drm_atomic_connector *drm_atomic_kms_connector(
    struct drm_atomic_kms *kms, uint32_t id)
{
    if (!kms)
        return NULL;
    for (uint32_t i = 0; i < kms->max_conn; i++)
        if (kms->current.connectors[i].used && kms->current.connectors[i].id == id)
            return &kms->current.connectors[i];
    return NULL;
}

const struct drm_atomic_plane *drm_atomic_kms_plane(struct drm_atomic_kms *kms,
                                                    uint32_t id)
{
    if (!kms)
        return NULL;
    for (uint32_t i = 0; i < kms->max_plane; i++)
        if (kms->current.planes[i].used && kms->current.planes[i].id == id)
            return &kms->current.planes[i];
    return NULL;
}

/* Helpers internos sobre snapshots */
static struct drm_atomic_crtc *snap_crtc(struct drm_atomic_snapshot *s,
                                         uint32_t id)
{
    for (int i = 0; i < DRM_ATOMIC_MAX_CRTCS; i++)
        if (s->crtcs[i].used && s->crtcs[i].id == id)
            return &s->crtcs[i];
    return NULL;
}

static struct drm_atomic_encoder *snap_enc(struct drm_atomic_snapshot *s,
                                           uint32_t id)
{
    for (int i = 0; i < DRM_ATOMIC_MAX_ENCODERS; i++)
        if (s->encoders[i].used && s->encoders[i].id == id)
            return &s->encoders[i];
    return NULL;
}

static struct drm_atomic_connector *snap_conn(struct drm_atomic_snapshot *s,
                                              uint32_t id)
{
    for (int i = 0; i < DRM_ATOMIC_MAX_CONNECTORS; i++)
        if (s->connectors[i].used && s->connectors[i].id == id)
            return &s->connectors[i];
    return NULL;
}

static struct drm_atomic_plane *snap_plane(struct drm_atomic_snapshot *s,
                                           uint32_t id)
{
    for (int i = 0; i < DRM_ATOMIC_MAX_PLANES; i++)
        if (s->planes[i].used && s->planes[i].id == id)
            return &s->planes[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Estado atômico                                                      */
/* ------------------------------------------------------------------ */

struct drm_atomic_state *drm_atomic_state_get(struct drm_device *dev)
{
    struct drm_atomic_kms *kms = drm_atomic_get_kms(dev);
    struct drm_atomic_state *st;

    if (!kms)
        return NULL;

    st = kmalloc(sizeof(struct drm_atomic_state));
    if (!st)
        return NULL;

    spin_lock(&kms->lock);
    st->old = kms->current;
    st->new = kms->current;
    spin_unlock(&kms->lock);

    st->kms = kms;
    st->committed = false;
    return st;
}

void drm_atomic_state_put(struct drm_atomic_state *state)
{
    if (state)
        kfree(state);
}

int drm_atomic_crtc_set_active(struct drm_atomic_state *st, uint32_t crtc_id,
                               bool active)
{
    struct drm_atomic_crtc *c;

    if (!st)
        return -EINVAL;
    c = snap_crtc(&st->new, crtc_id);
    if (!c)
        return -EINVAL;
    c->active = active;
    return 0;
}

int drm_atomic_crtc_set_mode(struct drm_atomic_state *st, uint32_t crtc_id,
                             uint32_t hdisplay, uint32_t vdisplay,
                             uint32_t vrefresh)
{
    struct drm_atomic_crtc *c;

    if (!st)
        return -EINVAL;
    c = snap_crtc(&st->new, crtc_id);
    if (!c)
        return -EINVAL;
    c->hdisplay = hdisplay;
    c->vdisplay = vdisplay;
    c->vrefresh = vrefresh;
    return 0;
}

int drm_atomic_crtc_set_fb(struct drm_atomic_state *st, uint32_t crtc_id,
                           uint32_t fb_handle, uint32_t width, uint32_t height,
                           uint32_t pitch)
{
    struct drm_atomic_crtc *c;

    if (!st)
        return -EINVAL;
    c = snap_crtc(&st->new, crtc_id);
    if (!c)
        return -EINVAL;
    c->fb_handle = fb_handle;
    (void)width;
    (void)height;
    (void)pitch;   /* dims relevantes ficam no plano/FB */
    return 0;
}

int drm_atomic_encoder_assign_crtc(struct drm_atomic_state *st, uint32_t enc_id,
                                   uint32_t crtc_id)
{
    struct drm_atomic_encoder *e;
    int i;

    if (!st)
        return -EINVAL;
    e = snap_enc(&st->new, enc_id);
    if (!e)
        return -EINVAL;

    /* Limpa o lado crtc de qualquer link antigo apontando para este encoder */
    for (i = 0; i < DRM_ATOMIC_MAX_CRTCS; i++) {
        if (st->new.crtcs[i].used &&
            st->new.crtcs[i].id != crtc_id &&
            st->new.crtcs[i].encoder_id == enc_id)
            st->new.crtcs[i].encoder_id = DRM_ATOMIC_NO_LINK;
    }

    if (crtc_id == DRM_ATOMIC_NO_LINK) {
        e->crtc_id = DRM_ATOMIC_NO_LINK;
        return 0;
    }

    {
        struct drm_atomic_crtc *c = snap_crtc(&st->new, crtc_id);

        if (!c)
            return -EINVAL;

        /* Exclusividade: um crtc só tem um encoder — rouba de outros */
        for (i = 0; i < DRM_ATOMIC_MAX_ENCODERS; i++) {
            if (st->new.encoders[i].used &&
                st->new.encoders[i].id != enc_id &&
                st->new.encoders[i].crtc_id == crtc_id) {
                st->new.encoders[i].crtc_id = DRM_ATOMIC_NO_LINK;

                /* limpa também o encoder_id do crtc roubado */
                for (int j = 0; j < DRM_ATOMIC_MAX_CRTCS; j++) {
                    if (st->new.crtcs[j].used &&
                        st->new.crtcs[j].id == crtc_id)
                        st->new.crtcs[j].encoder_id = DRM_ATOMIC_NO_LINK;
                }
            }
        }

        /* Link bidirecional: encoder aponta pro crtc e vice-versa */
        e->crtc_id = crtc_id;
        c->encoder_id = enc_id;
    }
    return 0;
}

int drm_atomic_encoder_set_possible_crtcs(struct drm_atomic_state *st,
                                          uint32_t enc_id, uint32_t mask)
{
    struct drm_atomic_encoder *e;

    if (!st)
        return -EINVAL;
    e = snap_enc(&st->new, enc_id);
    if (!e)
        return -EINVAL;
    e->possible_crtcs = mask;
    return 0;
}

int drm_atomic_connector_set_encoder(struct drm_atomic_state *st,
                                     uint32_t conn_id, uint32_t enc_id)
{
    struct drm_atomic_connector *c;

    if (!st)
        return -EINVAL;
    c = snap_conn(&st->new, conn_id);
    if (!c)
        return -EINVAL;
    c->encoder_id = enc_id;
    return 0;
}

int drm_atomic_plane_set_fb(struct drm_atomic_state *st, uint32_t plane_id,
                            uint32_t crtc_id, uint32_t fb_handle,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct drm_atomic_plane *p;

    if (!st)
        return -EINVAL;
    p = snap_plane(&st->new, plane_id);
    if (!p)
        return -EINVAL;
    p->crtc_id = crtc_id;
    p->fb_handle = fb_handle;
    p->x = x;
    p->y = y;
    p->width = w;
    p->height = h;
    return 0;
}

/* Slot do crtc dentro da tabela — usado pelo bitmask possible_crtcs */
static int crtc_slot_of(struct drm_atomic_snapshot *s, uint32_t crtc_id)
{
    for (int i = 0; i < DRM_ATOMIC_MAX_CRTCS; i++)
        if (s->crtcs[i].used && s->crtcs[i].id == crtc_id)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Propriedades nomeadas                                               */
/* ------------------------------------------------------------------ */

int drm_atomic_set_property(struct drm_atomic_state *st, uint32_t obj_id,
                            const char *name, uint64_t value)
{
    if (!st || !name)
        return -EINVAL;
    (void)obj_id;   /* props são globais por kms nesta fase (introspecção) */

    /* props ficam no kms (compartilhadas); escrita direta simples */
    struct drm_atomic_kms *kms = st->kms;
    spin_lock(&kms->lock);
    for (int t = 0; t < DRM_ATOMIC_MAX_CRTCS; t++) {
        for (int p = 0; p < DRM_ATOMIC_MAX_PROPS; p++) {
            struct drm_atomic_property *prop = &kms->crtc_props[t][p];
            if (prop->name[0] == '\0' || strcmp(prop->name, name) == 0) {
                strncpy(prop->name, name, DRM_ATOMIC_PROP_NAME_LEN - 1);
                prop->name[DRM_ATOMIC_PROP_NAME_LEN - 1] = '\0';
                prop->value = value;
                spin_unlock(&kms->lock);
                return 0;
            }
        }
    }
    spin_unlock(&kms->lock);
    return -ENOMEM;
}

uint64_t drm_atomic_get_property(struct drm_atomic_state *st, uint32_t obj_id,
                                 const char *name, bool *found)
{
    if (found)
        *found = false;
    if (!st || !name)
        return 0;
    (void)obj_id;

    struct drm_atomic_kms *kms = st->kms;
    spin_lock(&kms->lock);
    for (int t = 0; t < DRM_ATOMIC_MAX_CRTCS; t++) {
        for (int p = 0; p < DRM_ATOMIC_MAX_PROPS; p++) {
            struct drm_atomic_property *prop = &kms->crtc_props[t][p];
            if (prop->name[0] != '\0' && strcmp(prop->name, name) == 0) {
                if (found)
                    *found = true;
                spin_unlock(&kms->lock);
                return prop->value;
            }
        }
    }
    spin_unlock(&kms->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Validação / commit                                                  */
/* ------------------------------------------------------------------ */

bool drm_atomic_check_only(struct drm_atomic_state *st, const char **reason)
{
    struct drm_atomic_snapshot *n;

    if (reason)
        *reason = NULL;
    if (!st || !st->kms)
        return false;
    n = &st->new;

    /* CRTC ativo: FB + modo + encoder ligado */
    for (int i = 0; i < DRM_ATOMIC_MAX_CRTCS; i++) {
        struct drm_atomic_crtc *c = &n->crtcs[i];

        if (!c->used || !c->active)
            continue;

        if (!c->fb_handle) {
            if (reason)
                *reason = "crtc ativo sem FB";
            return false;
        }
        if (!c->hdisplay || !c->vdisplay || !c->vrefresh) {
            if (reason)
                *reason = "crtc ativo sem modo valido";
            return false;
        }
        if (c->encoder_id == DRM_ATOMIC_NO_LINK) {
            if (reason)
                *reason = "crtc ativo sem encoder";
            return false;
        }

        /* encoder precisa apontar de volta para este crtc e suportá-lo */
        struct drm_atomic_encoder *e = snap_enc(n, c->encoder_id);
        if (!e || e->crtc_id != c->id) {
            if (reason)
                *reason = "encoder não aponta para o crtc";
            return false;
        }
        int slot = crtc_slot_of(n, c->id);
        if (slot < 0 ||
            !(e->possible_crtcs & (1u << slot))) {
            if (reason)
                *reason = "encoder não suporta este crtc (possible_crtcs)";
            return false;
        }

        /* connector ligado ao encoder */
        bool have_conn = false;
        for (int j = 0; j < DRM_ATOMIC_MAX_CONNECTORS; j++) {
            struct drm_atomic_connector *cn = &n->connectors[j];

            if (cn->used && cn->encoder_id == c->encoder_id) {
                have_conn = cn->connected;
                break;
            }
        }
        if (!have_conn) {
            if (reason)
                *reason = "sem connector conectado ao encoder";
            return false;
        }
    }

    /* Planes: precisam pertencer a crtc ativo e caber no modo */
    for (int i = 0; i < DRM_ATOMIC_MAX_PLANES; i++) {
        struct drm_atomic_plane *p = &n->planes[i];

        if (!p->used || !p->fb_handle)
            continue;

        struct drm_atomic_crtc *c =
            (p->crtc_id == DRM_ATOMIC_NO_LINK)
                ? NULL : snap_crtc(n, p->crtc_id);

        if (!c || !c->active) {
            if (reason)
                *reason = "plane com FB em crtc inativo";
            return false;
        }
        if (p->x + p->width > c->hdisplay ||
            p->y + p->height > c->vdisplay) {
            if (reason)
                *reason = "plane maior que o modo do crtc";
            return false;
        }
    }

    return true;
}

int drm_atomic_commit(struct drm_atomic_state *st, const char **reason)
{
    int ret;

    if (!st || !st->kms)
        return -EINVAL;

    if (!drm_atomic_check_only(st, reason)) {
        /* ROLLBACK: nada foi instalado — estado corrente preservado */
        atomic_log("INFO", COLOR_BROWN,
                   "commit rejeitado: %s (rollback)",
                   (reason && *reason) ? *reason : "?");
        return -EINVAL;
    }

    /* Instalação atômica: uma cópia única sob o lock */
    spin_lock(&st->kms->lock);
    st->kms->current = st->new;
    spin_unlock(&st->kms->lock);

    st->committed = true;

    /* Hook opcional do driver */
    if (st->kms->funcs && st->kms->funcs->commit) {
        ret = st->kms->funcs->commit(st->kms->dev);
        if (ret) {
            /* driver recusou pós-validação → restaura old */
            spin_lock(&st->kms->lock);
            st->kms->current = st->old;
            spin_unlock(&st->kms->lock);
            st->committed = false;
            if (reason)
                *reason = "driver recusou o commit";
            return ret;
        }
    }

    atomic_log("INFO", COLOR_LIGHT_CYAN, "commit atomico aplicado");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Selftest de boot                                                    */
/* ------------------------------------------------------------------ */

int drm_atomic_test(void)
{
    static struct drm_device fake_dev;
    struct drm_atomic_kms *kms;
    uint32_t crtc_id, enc_id, conn_id, plane_id;
    const char *reason = NULL;
    int failures = 0;

    serial_print("[DRM-ATOMIC] Starting atomic KMS tests...\n");

    memset(&fake_dev, 0, sizeof(fake_dev));

    if (drm_atomic_kms_register(&fake_dev, 1, 1, 1, 1, NULL)) {
        atomic_log("FAIL", COLOR_LIGHT_RED, "kms_register falhou");
        return -EINVAL;
    }
    kms = drm_atomic_get_kms(&fake_dev);
    if (!kms) {
        atomic_log("FAIL", COLOR_LIGHT_RED, "get_kms retornou NULL");
        drm_atomic_kms_unregister(&fake_dev);
        return -EINVAL;
    }

    crtc_id   = kms->current.crtcs[0].id;
    enc_id    = kms->current.encoders[0].id;
    conn_id   = kms->current.connectors[0].id;
    plane_id  = kms->current.planes[0].id;

    /* ---- A) Pipeline completo válido → commit OK ---- */
    {
        struct drm_atomic_state *st = drm_atomic_state_get(&fake_dev);

        if (!st) { failures++; goto out; }

        drm_atomic_encoder_assign_crtc(st, enc_id, crtc_id);
        drm_atomic_connector_set_encoder(st, conn_id, enc_id);
        drm_atomic_crtc_set_mode(st, crtc_id, 1920, 1080, 60);
        drm_atomic_crtc_set_fb(st, crtc_id, 42, 1920, 1080, 1920 * 4);
        drm_atomic_crtc_set_active(st, crtc_id, true);
        drm_atomic_plane_set_fb(st, plane_id, crtc_id, 42,
                                0, 0, 1920, 1080);

        if (!drm_atomic_check_only(st, &reason)) {
            failures++;
            atomic_log("FAIL", COLOR_LIGHT_RED,
                       "A: estado valido rejeitado (%s)", reason ? reason : "?");
        } else if (drm_atomic_commit(st, &reason)) {
            failures++;
            atomic_log("FAIL", COLOR_LIGHT_RED,
                       "A: commit valido falhou (%s)", reason ? reason : "?");
        } else {
            const struct drm_atomic_crtc *c = drm_atomic_kms_crtc(kms, crtc_id);
            if (!c || !c->active || c->fb_handle != 42 ||
                c->hdisplay != 1920) {
                failures++;
                atomic_log("FAIL", COLOR_LIGHT_RED,
                           "A: readback divergente após commit");
            } else {
                atomic_log("PASS", COLOR_LIGHT_GREEN,
                           "commit atomico CRTC+connector aplicado");
            }
        }
        drm_atomic_state_put(st);
    }

    /* ---- B) Estado inválido → rollback preserva estado corrente ---- */
    {
        struct drm_atomic_state *st = drm_atomic_state_get(&fake_dev);

        /* tira o FB mantendo o crtc ativo → inválido */
        drm_atomic_crtc_set_fb(st, crtc_id, 0, 0, 0, 0);

        if (drm_atomic_commit(st, &reason) != -EINVAL || !reason) {
            failures++;
            atomic_log("FAIL", COLOR_LIGHT_RED,
                       "B: commit invalido deveria falhar");
        } else {
            const struct drm_atomic_crtc *c = drm_atomic_kms_crtc(kms, crtc_id);
            if (!c || c->fb_handle != 42 || !c->active) {
                failures++;
                atomic_log("FAIL", COLOR_LIGHT_RED,
                           "B: rollback não preservou estado anterior");
            } else {
                atomic_log("PASS", COLOR_LIGHT_GREEN,
                           "rollback: '%s' — estado anterior intacto",
                           reason);
            }
        }
        drm_atomic_state_put(st);
    }

    /* ---- C) Encoder sem suporte ao crtc (possible_crtcs) ---- */
    {
        struct drm_atomic_state *st = drm_atomic_state_get(&fake_dev);

        drm_atomic_encoder_set_possible_crtcs(st, enc_id, 0);

        if (drm_atomic_commit(st, &reason) != -EINVAL) {
            failures++;
            atomic_log("FAIL", COLOR_LIGHT_RED,
                       "C: encoder sem possible_crtcs deveria falhar");
        } else {
            atomic_log("PASS", COLOR_LIGHT_GREEN,
                       "validacao possible_crtcs OK ('%s')",
                       reason ? reason : "?");
        }
        drm_atomic_state_put(st);
    }

    /* ---- D) Plane maior que o modo ---- */
    {
        struct drm_atomic_state *st = drm_atomic_state_get(&fake_dev);

        drm_atomic_plane_set_fb(st, plane_id, crtc_id, 43,
                                0, 0, 2000, 1080);

        if (drm_atomic_commit(st, &reason) != -EINVAL) {
            failures++;
            atomic_log("FAIL", COLOR_LIGHT_RED,
                       "D: plane oversized deveria falhar");
        } else {
            atomic_log("PASS", COLOR_LIGHT_GREEN,
                       "validacao de plane vs modo OK ('%s')",
                       reason ? reason : "?");
        }
        drm_atomic_state_put(st);
    }

    /* ---- E) Propriedades nomeadas ---- */
    {
        struct drm_atomic_state *st = drm_atomic_state_get(&fake_dev);
        bool found = false;

        drm_atomic_set_property(st, crtc_id, "LINK_STATUS", 1);
        uint64_t v = drm_atomic_get_property(st, crtc_id,
                                             "LINK_STATUS", &found);
        if (!found || v != 1) {
            failures++;
            atomic_log("FAIL", COLOR_LIGHT_RED, "E: property roundtrip");
        } else {
            atomic_log("PASS", COLOR_LIGHT_GREEN,
                       "properties nomeadas OK (LINK_STATUS=%u)", (unsigned)v);
        }
        drm_atomic_state_put(st);
    }

out:
    drm_atomic_kms_unregister(&fake_dev);

    if (failures == 0) {
        atomic_log("PASS", COLOR_LIGHT_GREEN,
                   "Atomic KMS: ALL CHECKS PASSED");
        return 0;
    }
    atomic_log("FAIL", COLOR_LIGHT_RED,
               "Atomic KMS: %d check(s) FAILED", failures);
    return -EINVAL;
}
