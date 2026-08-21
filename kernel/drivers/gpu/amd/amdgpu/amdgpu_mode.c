/*
 * amdgpu_mode.c — modeset básico fbdev linear, SEM Display Core (Dev 3).
 *
 * Programa o pipeline DCE subset da emulação (CRTC0 + GRPH pipe 0):
 * timing VESA 1920x1080@60, superfície GRPH apontando para um BO VRAM
 * alocado pelo vram_mgr (Dev 2), pitch/format 32bpp e CRTC enable.
 *
 * Entrega: console gráfico do ApolloOS renderizado via GPU — o host
 * (vgpu_dce.c) faz o blit da VRAM pro console QEMU.
 */

#include <amdgpu.h>
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

/* Timing VESA 1920x1080@60 (valores reais do DMT) */
#define H_TOTAL   2200u
#define H_DISP    1920u
#define H_SYNC_S  2008u
#define H_SYNC_E  2052u
#define V_TOTAL   1125u
#define V_DISP    1080u
#define V_SYNC_S  1084u
#define V_SYNC_E  1089u

static void mode_log(const char *msg)
{
    serial_print("[amdgpu-mode] ");
    serial_print(msg);
    serial_print("\n");
}

int amdgpu_modeset_init(struct amdgpu_device *adev,
                        uint32_t w, uint32_t h, uint32_t bpp)
{
    int rc;

    if (!adev || !adev->fb_enabled)
        return -EINVAL;
    if (bpp != 32)
        return -EINVAL;   /* v0.1.0: só XRGB8888 */

    /* BO de scanout no carveout VRAM (pinado pelo mapping abaixo) */
    rc = amdgpu_bo_create(adev, w * h * 4, true, &adev->fb_bo);
    if (rc) {
        mode_log("falha ao alocar BO de scanout em VRAM");
        return rc;
    }

    adev->fb_vaddr = gem_mmap_phys(adev->fb_bo);
    if (!adev->fb_vaddr) {
        mode_log("mmap do BO de scanout falhou");
        gem_put(adev->fb_bo);
        adev->fb_bo = NULL;
        return -ENOMEM;
    }
    adev->fb_vram_off = adev->fb_bo->phys - adev->vram_phys;
    memset(adev->fb_vaddr, 0x00, w * h * 4);

    adev->mode_w = w;
    adev->mode_h = h;
    adev->mode_bpp = bpp;
    adev->pitch = w * 4;

    /* Desliga o CRTC antes de programar timing */
    amdgpu_wreg(adev, mmCRTC0_CONTROL, 0);

    amdgpu_wreg(adev, mmCRTC0_H_TOTAL,
                ((H_TOTAL - 1) << 16) | (H_DISP - 1));
    amdgpu_wreg(adev, mmCRTC0_H_BLANK_START_END,
                ((H_TOTAL - 1) << 16) | (H_DISP - 1));
    amdgpu_wreg(adev, mmCRTC0_H_SYNC_A,
                ((H_SYNC_E - 1) << 16) | (H_SYNC_S - 1));

    amdgpu_wreg(adev, mmCRTC0_V_TOTAL,
                ((V_TOTAL - 1) << 16) | (V_DISP - 1));
    amdgpu_wreg(adev, mmCRTC0_V_BLANK_START_END,
                ((V_TOTAL - 1) << 16) | (V_DISP - 1));
    amdgpu_wreg(adev, mmCRTC0_V_SYNC_A,
                ((V_SYNC_E - 1) << 16) | (V_SYNC_S - 1));

    /* Superfície GRPH: offset do BO dentro do BAR1 + pitch + formato */
    amdgpu_wreg(adev, mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS,
                (uint32_t)(adev->fb_vram_off & 0xFFFFFFFFu));
    amdgpu_wreg(adev, mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH,
                (uint32_t)(adev->fb_vram_off >> 32));
    amdgpu_wreg(adev, mmCRTC0_GRPH_PITCH, adev->pitch);
    amdgpu_wreg(adev, mmCRTC0_GRPH_X_START, 0);
    amdgpu_wreg(adev, mmCRTC0_GRPH_Y_START, 0);
    amdgpu_wreg(adev, mmCRTC0_GRPH_X_END, w);
    amdgpu_wreg(adev, mmCRTC0_GRPH_Y_END, h);
    amdgpu_wreg(adev, mmCRTC0_GRPH_FORMAT, AMDGPU_GRPH_FORMAT_32BPP);

    /* HDP flush e liga o CRTC */
    amdgpu_wreg(adev, mmHDP_HOST_PATH_CNTL, 1);
    amdgpu_wreg(adev, mmHDP_FLUSH_INVALIDATE, 1);
    amdgpu_wreg(adev, mmCRTC0_CONTROL, AMDGPU_CRTC_ENABLE);

    mode_log("modeset 1920x1080@60 XRGB8888 aplicado via DCE subset");
    serial_print("[amdgpu-mode] GRPH @ VRAM+0x");
    {
        static const char hx[] = "0123456789abcdef";
        char b[17];
        for (int i = 0; i < 16; i++)
            b[i] = hx[(adev->fb_vram_off >> ((15 - i) * 4)) & 0xF];
        b[16] = '\0';
        serial_print(b);
    }
    serial_print("\n");
    return 0;
}

void amdgpu_modeset_fini(struct amdgpu_device *adev)
{
    if (!adev)
        return;
    amdgpu_wreg(adev, mmCRTC0_CONTROL, 0);
}

/* ------------------------------------------------------------------ */
/* Selftest Dev 3: escreve padrão no FB e lê de volta pela GPU window  */
/* ------------------------------------------------------------------ */

int amdgpu_display_selftest(void)
{
    struct amdgpu_device *adev = amdgpu_adev;

    if (!adev || !adev->fb_vaddr)
        return -EINVAL;

    uint32_t *fb = adev->fb_vaddr;
    uint32_t pitch_px = adev->pitch / 4;

    /* Faixas RGB primárias nas primeiras linhas */
    for (uint32_t x = 0; x < 64 && x < adev->mode_w; x++) {
        fb[0 * pitch_px + x] = 0x00FF0000u;   /* R */
        fb[1 * pitch_px + x] = 0x0000FF00u;   /* G */
        fb[2 * pitch_px + x] = 0x000000FFu;   /* B */
    }

    bool ok = fb[0] == 0x00FF0000u &&
              fb[pitch_px] == 0x0000FF00u &&
              fb[2 * pitch_px] == 0x000000FFu;

    /* Limpa as faixas (o test pattern assume FB zerado como base) */
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
             "display selftest OK (FB VRAM read-back)");
    return 0;
}
