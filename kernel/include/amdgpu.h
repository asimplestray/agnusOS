/*
 * amdgpu.h — amdgpu v0.1.0 MINIMAL (sem DC) para ApolloOS.
 *
 * Fase 3 do port GPU (ver GPU_PORTING_TASKS.md):
 *   Dev 1 — HW init + ASIC detect + rmmio + reg dump      (amdgpu_device.c)
 *   Dev 2 — VRAM/GTT managers sobre o GEM nativo          (amdgpu_vram_mgr.c)
 *   Dev 3 — modeset fbdev linear via DCE subset           (amdgpu_mode.c)
 *   Dev 4 — GFX ring (CP) + test pattern + thermal monitor(amdgpu_gfx.c,
 *                                                           gpu_test_pattern.c,
 *                                                           thermal_monitor.c)
 *
 * Escopo v0.1.0: Polaris (VI/GFX8) contra a emulação vgpu amd-rx480.
 * Sem Display Core (DC), sem DPM real, sem compute/SDMA — só GFX ring.
 */

#ifndef _AMDGPU_H_
#define _AMDGPU_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <screen.h>
#include <spinlock.h>
#include <workqueue.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_sched.h>

struct dc_context;

/* ---- Identificação ---- */
#define AMDGPU_VENDOR_ID        0x1002

enum amdgpu_asic_type {
    CHIP_UNKNOWN = 0,
    CHIP_POLARIS10,     /* RX 470/480 — 0x67DF */
    CHIP_POLARIS11,     /* RX 570/580 — 0x67FF/0x67EF */
    CHIP_POLARIS12,     /* RX 590    — 0x6987 */
    CHIP_VEGA10,        /* Vega 56/64 (DCN 1.x) — reservado Fase 4+ */
    CHIP_NAVI22,        /* RX 6700 XT — 0x73DF (DCN 2.x) */
};

struct amdgpu_asic_entry {
    uint16_t    dev_id;
    enum amdgpu_asic_type type;
    const char *name;
};

/* ---- Janelas de VMM para BARs (fora das usadas por GEM/dma-buf/polaris) */
#define AMDGPU_RMMIO_VADDR  0xFFFF800600000000ULL   /* BAR0 (16 MB) */
#define AMDGPU_VRAM_VADDR   0xFFFF800700000000ULL   /* BAR1         */
#define AMDGPU_MMIO_SIZE    (16u * 1024u * 1024u)

/* ---- Registros (BAR0/rmmio) — subconjunto implementado pela vgpu ---- */
#define mmCONFIG_MEMSIZE            0x00005428
#define mmGRBM_STATUS               0x00008010
#define mmGRBM_STATUS2              0x00008014
#define mmGRBM_SOFT_RESET           0x00008020
#define mmSRBM_STATUS               0x00000E50
#define mmBIF_FB_EN                 0x00000504
#define mmHDP_HOST_PATH_CNTL        0x00002C00
#define mmHDP_FLUSH_INVALIDATE      0x00002C10
#define mmSMC_MSG                   0x00000228
#define mmSMC_MSG_ARG               0x0000022C
#define mmSMC_RESP                  0x00000230
#define mmGFX_RB_RPTR               0x00000800
#define mmGFX_RB_WPTR               0x00000804

/* DCE subset da emulação (timing + GRPH pipe 0) */
#define mmCRTC0_H_TOTAL             0x0006500
#define mmCRTC0_H_BLANK_START_END   0x0006504
#define mmCRTC0_H_SYNC_A            0x0006508
#define mmCRTC0_V_TOTAL             0x0006510
#define mmCRTC0_V_BLANK_START_END   0x0006514
#define mmCRTC0_V_SYNC_A            0x0006518
#define mmCRTC0_CONTROL             0x001B0AC   /* bit0 = enable */
#define mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS        0x001B100
#define mmCRTC0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH   0x001B104
#define mmCRTC0_GRPH_PITCH          0x001B108
#define mmCRTC0_GRPH_X_START        0x001B10C
#define mmCRTC0_GRPH_Y_START        0x001B110
#define mmCRTC0_GRPH_X_END          0x001B114
#define mmCRTC0_GRPH_Y_END          0x001B118
#define mmCRTC0_GRPH_FORMAT         0x001B11C

/* HPD (Fase 4): reflete a property `hpd_connected` da vgpu */
#define mmHPD0_STATUS               0x001B200

#define AMDGPU_CRTC_ENABLE          0x1u
#define AMDGPU_GRPH_FORMAT_32BPP    4u

/* SMC messages (subconjunto; emulação responde RESP=1 e ARG=0) */
#define AMDGPU_SMC_MSG_ReadTemperature  0x18
#define AMDGPU_SMC_RESP_OK              1

/* Thermal policy (doc GPU_PORTING_STRATEGY §4) */
#define AMDGPU_THERMAL_POLL_TICKS   200     /* 2 s @ 100 Hz */
#define AMDGPU_THERMAL_WARN_C       85
#define AMDGPU_THERMAL_CRIT_C       95

/* Power state forçado até existir DPM real (Fase 5) */
#define AMD_DPM_FORCED_LEVEL_LOW    1u

/* GFX ring */
#define AMDGPU_RING_SIZE_DW         4096    /* 16 KB */
#define AMDGPU_GFX_OPCODE_NOP       0x10
#define AMDGPU_GFX_OPCODE_WRITE_DATA 0x37
#define AMDGPU_GFX_OPCODE_FENCE     0x99    /* exclusivo da emulação */

#define AMDGPU_PKT_HDR(count, op) \
    (0xC0000000u | (((uint32_t)(count)) << 16) | (((uint32_t)(op)) << 8))

/* ---- Estrutura principal do adaptador ---- */
struct amdgpu_device {
    /* PCI */
    uint8_t     bus, dev, func;
    uint16_t    dev_id;

    /* ASIC */
    enum amdgpu_asic_type asic_type;
    const char *asic_name;
    uint32_t    revision;

    /* BAR0 — rmmio */
    uint64_t    rmmio_phys;
    uint64_t    rmmio_virt;

    /* BAR1 — VRAM */
    uint64_t    vram_phys;      /* base física do BAR1 */
    uint64_t    vram_virt;      /* janela mapeada      */
    size_t      vram_size;

    bool        fb_enabled;
    bool        initialized;

    /* Gerência de memória (GEM nativo como VRAM/GTT mgr — Dev 2) */
    struct drm_device ddev;     /* identidade p/ o gerenciador GEM */

    /* Display (Dev 3 / Fase 4: DC nativo + fallback fbdev) */
    uint32_t    mode_w, mode_h, mode_bpp, pitch;
    struct drm_gem_object *fb_bo;       /* buffer A (scanout)          */
    struct drm_gem_object *fb_bo_b;     /* buffer B (flip, Fase 4)     */
    void       *fb_vaddr;
    uint64_t    fb_vram_off;    /* offset do BO dentro do BAR1     */
    bool        use_dc;         /* display core ativo (senão fbdev) */
    struct dc_context *dc_ctx;

    /* GFX ring (Dev 4) */
    struct drm_gem_object *ring_bo;
    uint32_t   *ring_cpu;       /* mapping kernel do ring          */
    uint32_t    ring_wptr;
    spinlock_irq_t ring_lock;

    /* Scheduler DRM (integração dos jobs do ring) */
    struct drm_gpu_scheduler sched;
    struct drm_sched_entity entity;

    /* PM / térmico / pattern (Dev 4) */
    uint32_t    dpm_forced_level;   /* LOW até a Fase 5 */
    int         last_temp_c;
    volatile bool thermal_running;
    uint64_t    thermal_last_tick;
    volatile bool pattern_enabled;
    uint64_t    pattern_last_tick;
    uint32_t    frame_count;
};

/* RREG32/WREG32 via rmmio (BAR0 é MMIO direto na emulação) */
static inline uint32_t amdgpu_rreg(struct amdgpu_device *adev, uint32_t reg)
{
    return *(volatile uint32_t *)(uintptr_t)(adev->rmmio_virt + reg);
}

static inline void amdgpu_wreg(struct amdgpu_device *adev, uint32_t reg,
                               uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(adev->rmmio_virt + reg) = val;
}

/* Log compartilhado dos sub-arquivos (serial + tela) */
void adev_log(const char *tag, vga_color_t color, const char *fmt, ...);

/* ---- Dev 1 — device init ---- */
int  amdgpu_init(void);                 /* probe PCI + cadeia completa */
void amdgpu_fini(void);
extern struct amdgpu_device *amdgpu_adev;   /* singleton (v0.1.0: 1 GPU) */

/* ---- Dev 2 — memória ---- */
int  amdgpu_vram_mgr_init(struct amdgpu_device *adev);
void amdgpu_vram_mgr_fini(struct amdgpu_device *adev);
int  amdgpu_bo_create(struct amdgpu_device *adev, size_t size, bool in_vram,
                      struct drm_gem_object **out);
int  amdgpu_mem_selftest(void);

/* ---- Dev 3 — display ---- */
int  amdgpu_modeset_init(struct amdgpu_device *adev,
                         uint32_t w, uint32_t h, uint32_t bpp);
void amdgpu_modeset_fini(struct amdgpu_device *adev);
int  amdgpu_display_selftest(void);

/* ---- Dev 4 — gfx / pattern / thermal ---- */
int  amdgpu_gfx_init(struct amdgpu_device *adev);
void amdgpu_gfx_fini(struct amdgpu_device *adev);
int  amdgpu_ring_submit_write_fb(struct amdgpu_device *adev,
                                 uint64_t fb_off_bytes, uint32_t value);
int  amdgpu_gfx_selftest(void);

int  amdgpu_gpu_test_pattern_start(void);
void amdgpu_gpu_test_pattern_stop(void);

int  amdgpu_thermal_monitor_start(void);
void amdgpu_thermal_monitor_stop(void);
int  amdgpu_smu_read_temp(struct amdgpu_device *adev);
void amdgpu_thermal_poll_from_idle(struct amdgpu_device *adev);

/* Motor dos serviços periódicos — chamar do loop idle do kernel */
void amdgpu_idle_tick(void);

#endif /* _AMDGPU_H_ */
