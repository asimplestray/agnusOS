/*
 * gpu_test_pattern.c — test pattern HSV animado (Dev 4).
 *
 * Desenha um gradiente HSV em rotação contínua no framebuffer de
 * scanout (BO VRAM do modeset), ~10 fps limitados por ticks do PIT.
 *
 * Entrega da tarefa: pattern 24/7 sem hang. Execução: loop idle do
 * kernel via amdgpu_idle_tick() — a v0.1.0 não tem worker threads e o
 * workqueue nativo é passivo. O bulk dos pixels vai por cópia CPU até
 * existir SDMA (Fase 5); o GFX ring sinaliza o fence de frame.
 */

#include <amdgpu.h>
#include <timer.h>
#include <string.h>
#include <serial.h>
#include <screen.h>

#define PATTERN_FRAME_TICKS 10u     /* ~10 fps @ PIT 100 Hz */
#define PATTERN_HUE_BANDS   24u     /* bandas verticais de matiz    */

static uint32_t pattern_palette[256];
static bool palette_ready;

static void pattern_build_palette(void)
{
    for (int i = 0; i < 256; i++) {
        int h = (i * 360) >> 8;
        int region = h / 60;
        if (region > 5)
            region = 5;
        int rem = ((h % 60) << 8) / 60;
        int v = 200;

        int p = 0;
        int q = (v * (255 - rem)) >> 8;
        int t = (v * (255 - (255 - rem))) >> 8;

        uint8_t r = 0, g = 0, b = 0;
        switch (region) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
        pattern_palette[i] = ((uint32_t)r << 16) |
                             ((uint32_t)g << 8) | (uint32_t)b;
    }
    palette_ready = true;
}

/* Um frame completo do gradiente diagonal animado */
static void pattern_draw_frame(struct amdgpu_device *adev)
{
    uint32_t *fb = adev->fb_vaddr;
    uint32_t pitch_px = adev->pitch / 4;
    uint8_t hue_shift = (uint8_t)(adev->frame_count * 5);

    if (!palette_ready)
        return;

    for (uint32_t y = 0; y < adev->mode_h; y++) {
        uint32_t band_y = (y * PATTERN_HUE_BANDS) / adev->mode_h;
        uint32_t *line = fb + y * pitch_px;

        for (uint32_t x = 0; x < adev->mode_w; x++) {
            uint32_t band_x = (x * PATTERN_HUE_BANDS) / adev->mode_w;
            uint8_t idx = (uint8_t)(band_x + band_y + hue_shift);
            line[x] = 0xFF000000u | pattern_palette[idx];
        }
    }

    /* fence de frame pelo GFX ring (plumagem CP honesta na emulação) */
    amdgpu_ring_submit_write_fb(adev,
                                (uint64_t)adev->frame_count & 0x3Full,
                                adev->frame_count);
    adev->frame_count++;
}

int amdgpu_gpu_test_pattern_start(void)
{
    struct amdgpu_device *adev = amdgpu_adev;

    if (!adev || !adev->initialized || !adev->fb_vaddr)
        return -EINVAL;
    if (!palette_ready)
        pattern_build_palette();

    adev->pattern_enabled = true;
    adev->pattern_last_tick = timer_get_ticks();
    serial_print("[amdgpu-pattern] ON (~10fps idle-driven)\n");
    return 0;
}

void amdgpu_gpu_test_pattern_stop(void)
{
    struct amdgpu_device *adev = amdgpu_adev;

    if (!adev)
        return;
    adev->pattern_enabled = false;
    serial_print("[amdgpu-pattern] OFF\n");
}

/* ------------------------------------------------------------------ */
/* Loop idle: motor dos serviços periódicos da v0.1.0                  */
/* ------------------------------------------------------------------ */

void amdgpu_idle_tick(void)
{
    struct amdgpu_device *adev = amdgpu_adev;
    uint64_t now;

    if (!adev || !adev->initialized)
        return;

    now = timer_get_ticks();

    if (adev->thermal_running &&
        now - adev->thermal_last_tick >= AMDGPU_THERMAL_POLL_TICKS) {
        extern void amdgpu_thermal_poll_from_idle(struct amdgpu_device *a);
        adev->thermal_last_tick = now;
        amdgpu_thermal_poll_from_idle(adev);
    }

    if (adev->pattern_enabled &&
        now - adev->pattern_last_tick >= PATTERN_FRAME_TICKS) {
        adev->pattern_last_tick = now;
        pattern_draw_frame(adev);
    }
}
