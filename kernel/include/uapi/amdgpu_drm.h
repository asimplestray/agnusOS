/* uapi/amdgpu_drm.h — ABI amdgpu userspace (uAPI 1.0)
 *
 * Subset estável para amdgpu v0.1.0 MINIMAL (Polaris 0x67DF/0x6FDF) + DC.
 * Congelado antes de Fase 5 (DPM/compute). Baseado em drivers/gpu/drm/amd 6.6
 * + kernel/include/amdgpu.h:50 + kernel/drivers/gpu/amd/amdgpu/
 *
 * Regra: structs têm version/pad para extensão sem quebrar ABI.
 */

#ifndef _UAPI_AMDGPU_DRM_H_
#define _UAPI_AMDGPU_DRM_H_

#include <stdint.h>
#include "drm.h"

#define AMDGPU_UAPI_VERSION_MAJOR 1
#define AMDGPU_UAPI_VERSION_MINOR 0

/* Device IDs estáveis (amdgpu_asic_table) */
#define AMDGPU_CHIP_POLARIS10   0x67DF
#define AMDGPU_CHIP_POLARIS11   0x67EF
#define AMDGPU_CHIP_POLARIS12   0x6987
#define AMDGPU_CHIP_POLARIS20   0x6FDF  /* RX 590 GME 8GB — real HW test 8192M */
#define AMDGPU_CHIP_NAVI22      0x73DF

/* GEM — amdgpu BO (kernel/drivers/gpu/amd/amdgpu/amdgpu_vram_mgr.c:1) */
#define AMDGPU_GEM_DOMAIN_GTT   GEM_DOMAIN_GTT
#define AMDGPU_GEM_DOMAIN_VRAM  GEM_DOMAIN_VRAM
#define AMDGPU_GEM_CREATE       DRM_IOCTL_GEM_CREATE /* alias */

struct drm_amdgpu_gem_create_in {
    uint64_t size;      /* bytes */
    uint64_t alignment; /* bytes, 0 = default 4K */
    uint32_t domain;    /* AMDGPU_GEM_DOMAIN_* */
    uint32_t flags;     /* pad */
};

struct drm_amdgpu_gem_create_out {
    uint32_t handle;
    uint32_t _pad;
    uint64_t offset;    /* VRAM offset */
};

/* GFX ring — PACKET3 (kernel/include/amdgpu.h:129) */
#define AMDGPU_GFX_OPCODE_NOP        0x10
#define AMDGPU_GFX_OPCODE_WRITE_DATA 0x37
#define AMDGPU_GFX_OPCODE_FENCE      0x99  /* emulação; HW real usa EVENT_WRITE_EOP */

#define AMDGPU_PKT_HDR(count, op) (0xC0000000u | (((uint32_t)(count)) << 16) | (((uint32_t)(op)) << 8))
#define AMDGPU_RING_SIZE_DW      4096

/* FW — polaris10 blobs (scripts/make_fw_initrd.sh:8) */
#define AMDGPU_FW_PFP   "amdgpu/polaris10_pfp.bin"  /* 17044 v0xeb */
#define AMDGPU_FW_ME    "amdgpu/polaris10_me.bin"   /* 17044 v0xa1 */
#define AMDGPU_FW_CE    "amdgpu/polaris10_ce.bin"   /* 8852  v0x86 */
#define AMDGPU_FW_RLC   "amdgpu/polaris10_rlc.bin"
#define AMDGPU_FW_MEC   "amdgpu/polaris10_mec.bin"
#define AMDGPU_FW_SDMA  "amdgpu/polaris10_sdma.bin"

/* Display — DC (kernel/include/amdgpu_dc.h) */
#define AMDGPU_DC_MAX_CRTC  4
#define AMDGPU_DC_MAX_PLANE 4

struct drm_amdgpu_mode {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;       /* 32 = XRGB8888 */
    uint32_t refresh;   /* Hz */
};

/* Info — rmmio/vram addrs (kernel/include/amdgpu.h:50) */
#define AMDGPU_INFO_RMMIO_VADDR  0xFFFF800600000000ULL
#define AMDGPU_INFO_VRAM_VADDR   0xFFFF800700000000ULL
#define AMDGPU_INFO_MMIO_SIZE    (16ULL*1024*1024)
#define AMDGPU_INFO_VRAM_SIZE    (256ULL*1024*1024) /* aperture, real 8GB via CONFIG_MEMSIZE */

/* Ioctls amdgpu — base 0x40 + offset (nunca mudar) */
#define DRM_AMDGPU_GEM_CREATE   _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE+0x00, struct drm_amdgpu_gem_create_in)
#define DRM_AMDGPU_CS           _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE+0x10, struct drm_amdgpu_cs)
#define DRM_AMDGPU_MODE_SET     _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE+0x20, struct drm_amdgpu_mode)

struct drm_amdgpu_cs {
    uint32_t handle;    /* GEM handle do IB */
    uint32_t _pad;
    uint64_t ib_offset;
    uint64_t ib_bytes;
    uint32_t fence;     /* out, seqno */
};

#endif /* _UAPI_AMDGPU_DRM_H_ */
