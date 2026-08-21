/*
 * amdgpu_mode.c — display pipeline (Fase 4).
 *
 * Caminho primário: DC nativo (amdgpu_dc) — stream montado e commitado
 * pelo resource pool da família do ASIC, com double-buffer para flips
 * sem tearing. Fallback: se o DC não subir (pool ausente / HPD off no
 * boot), o caminho direto de registros da Fase 3 permanece como
 * "crash path" fbdev.
 *
 * Integração atomic KMS: drm_atomic_funcs.commit (Fase 2) é o hook que
 * um futuro DC real consumirá; hoje dc_commit_streams cumpre esse papel.
 */

#include <amdgpu.h>
#include <amdgpu_dc.h>
#include <drm/drm_gem.h>
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

/* Stream persistente do scanout (o DC guarda ponteiro em ctx->current) */
static struct dc_stream_state fb_stream;

static void mode_log(const char *msg)
{
    serial_print("[amdgpu-mode] ");
    serial_print(msg);
    serial_print("\n");
}

/* ------------------------------------------------------------------ */
/* Caminho legado (fallback fbdev — crash path)                        */
/* ------------------------------------------------------------------ */

static int fbdev_direct_modeset(struct amdgpu_device *adev,
                                uint32_t w, uint32_t h)
{
    const uint32_t H_TOTAL = 2200u, V_TOTAL = 1125u;
    const uint32_t H_SYNC_S = 2008u, H_SYNC_E = 2052u;
    const uint32_t V_SYNC_S = 1084u, V_SYNC_E = 1089u;

    amdgpu_wreg(adev, mmCRTC0_CONTROL, 0);

    amdgpu_wreg(adev, mmCRTC0_H_TOTAL, ((H_TOTAL - 1) << 16) | (w - 1));
    amdgpu_wreg(adev, mmCRTC0_H_BLANK_START_END,
                ((H_TOTAL - 1) << 16) | (w - 1));
    amdgpu_wreg(adev, mmCRTC0_H_SYNC_A,
                ((H_SYNC_E - 1) << 16) | (H_SYNC_S - 1));
    amdgpu_wreg(adev, mmCRTC0_V_TOTAL, ((V_TOTAL - 1) << 16) | (h - 1));
    amdgpu_wreg(adev, mmCRTC0_V_BLANK_START_END,
                ((V_TOTAL - 1) << 16) | (h - 1));
    amdgpu_wreg(adev, mmCRTC0_V_SYNC_A,
                ((V_SYNC_E - 1) << 16) | (V_SYNC_S - 1));

    amdgpu_wreg(adev, mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS,
                (uint32_t)(adev->fb_vram_off & 0xFFFFFFFFu));
    amdgpu_wreg(adev, mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH,
                (uint32_t)(adev->fb_vram_off >> 32));
    amdgpu_wreg(adev, mmCRTC0_GRPH_PITCH, w * 4);
    amdgpu_wreg(adev, mmCRTC0_GRPH_X_START, 0);
    amdgpu_wreg(adev, mmCRTC0_GRPH_Y_START, 0);
    amdgpu_wreg(adev, mmCRTC0_GRPH_X_END, w);
    amdgpu_wreg(adev, mmCRTC0_GRPH_Y_END, h);
    amdgpu_wreg(adev, mmCRTC0_GRPH_FORMAT, AMDGPU_GRPH_FORMAT_32BPP);

    amdgpu_wreg(adev, mmHDP_HOST_PATH_CNTL, 1);
    amdgpu_wreg(adev, mmHDP_FLUSH_INVALIDATE, 1);
    amdgpu_wreg(adev, mmCRTC0_CONTROL, AMDGPU_CRTC_ENABLE);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

int amdgpu_modeset_init(struct amdgpu_device *adev,
                        uint32_t w, uint32_t h, uint32_t bpp)
{
    int rc;

    if (!adev || !adev->fb_enabled || bpp != 32)
        return -EINVAL;

    /* Double-buffer: A em scanout, B pronto p/ flip (Fase 4 Dev 4) */
    rc = amdgpu_bo_create(adev, w * h * 4, true, &adev->fb_bo);
    if (rc) {
        mode_log("falha ao alocar FB A");
        return rc;
    }
    if (amdgpu_bo_create(adev, w * h * 4, true, &adev->fb_bo_b)) {
        gem_put(adev->fb_bo);
        adev->fb_bo = NULL;
        mode_log("falha ao alocar FB B");
        return -ENOMEM;
    }
    adev->fb_vaddr = gem_mmap_phys(adev->fb_bo);
    void *vb = gem_mmap_phys(adev->fb_bo_b);
    if (!adev->fb_vaddr || !vb) {
        mode_log("mmap dos FBs falhou");
        return -ENOMEM;
    }
    memset(adev->fb_vaddr, 0x00, w * h * 4);
    memset(vb, 0x00, w * h * 4);

    adev->mode_w = w;
    adev->mode_h = h;
    adev->mode_bpp = bpp;
    adev->pitch = w * 4;

    {
        struct drm_gem_object *a = adev->fb_bo;
        adev->fb_vram_off = a->phys - adev->vram_phys;
    }

    /* ---- DC primeiro ---- */
    if (dc_create(adev, &adev->dc_ctx) == 0 &&
        adev->dc_ctx->links[0].connected) {

        memset(&fb_stream, 0, sizeof(fb_stream));
        fb_stream.hdisplay = w;
        fb_stream.vdisplay = h;
        fb_stream.vrefresh = 60;
        fb_stream.plane.surface_vram_off = adev->fb_vram_off;
        fb_stream.plane.width = w;
        fb_stream.plane.height = h;
        fb_stream.plane.format = AMDGPU_GRPH_FORMAT_32BPP;
        fb_stream.link = &adev->dc_ctx->links[0];

        if (dc_commit_streams(adev->dc_ctx, &fb_stream) == 0) {
            adev->use_dc = true;
            mode_log("DC commit OK — scanout via Display Core");
            serial_print("[amdgpu-mode] pool=");
            serial_print(adev->dc_ctx->pool->name);
            serial_print("\n");
            return 0;
        }
        mode_log("dc_commit falhou — usando fallback fbdev");
    } else {
        mode_log("DC indisponível — usando fallback fbdev");
    }

    /* ---- Fallback fbdev (crash path) ---- */
    adev->use_dc = false;
    return fbdev_direct_modeset(adev, w, h);
}

void amdgpu_modeset_fini(struct amdgpu_device *adev)
{
    if (!adev)
        return;

    if (adev->use_dc && adev->dc_ctx && adev->dc_ctx->current)
        adev->dc_ctx->pool->set_power(adev, false);
    else
        amdgpu_wreg(adev, mmCRTC0_CONTROL, 0);

    dc_destroy(&adev->dc_ctx);
    adev->use_dc = false;

    if (adev->fb_bo) {
        gem_unmap_phys(adev->fb_bo);
        gem_put(adev->fb_bo);
        adev->fb_bo = NULL;
    }
    if (adev->fb_bo_b) {
        gem_unmap_phys(adev->fb_bo_b);
        gem_put(adev->fb_bo_b);
        adev->fb_bo_b = NULL;
    }
    adev->fb_vaddr = NULL;
}

/* ------------------------------------------------------------------ */
/* Selftest Dev 3/Fase 4: read-back do FB ativo                        */
/* ------------------------------------------------------------------ */

int amdgpu_display_selftest(void)
{
    struct amdgpu_device *adev = amdgpu_adev;

    if (!adev || !adev->fb_vaddr)
        return -EINVAL;

    uint32_t *fb = adev->fb_vaddr;
    uint32_t pitch_px = adev->pitch / 4;

    for (uint32_t x = 0; x < 64 && x < adev->mode_w; x++) {
        fb[0 * pitch_px + x] = 0x00FF0000u;
        fb[1 * pitch_px + x] = 0x0000FF00u;
        fb[2 * pitch_px + x] = 0x000000FFu;
    }

    bool ok = fb[0] == 0x00FF0000u &&
              fb[pitch_px] == 0x0000FF00u &&
              fb[2 * pitch_px] == 0x000000FFu;

    for (uint32_t x = 0; x < 64 && x < adev->mode_w; x++) {
        fb[0 * pitch_px + x] = 0;
        fb[1 * pitch_px + x] = 0;
        fb[2 * pitch_px + x] = 0;
    }

    if (!ok) {
        adev_log("FAIL", COLOR_LIGHT_RED,
                 "display selftest: read-back divergente");
        return -EINVAL;
    }
    adev_log("PASS", COLOR_LIGHT_GREEN,
             "display selftest OK (%s)",
             adev->use_dc ? "scanout via DC" : "fallback fbdev");
    return 0;
}
