/* uapi/drm.h — ABI estável userspace/kernel para DRM (AgnusOS uAPI 1.0)
 *
 * Congelado em PRE_AMDGPU_TASKS.md:P3 antes de copiar drivers/gpu/drm/amd 6.6.
 * Regra: nunca quebrar numeric ABI; novas features via novo ioctl ou extensão
 * com version field. Compat com libdrm/mesa radv.
 *
 * Baseado em kernel/include/drm/drm_driver.h:82 + drm_device.h:69
 * Versão: 1.0 (2026-09-04) — tag uapi-1.0
 */

#ifndef _UAPI_DRM_H_
#define _UAPI_DRM_H_

#include <stdint.h>

#define DRM_UAPI_VERSION_MAJOR 1
#define DRM_UAPI_VERSION_MINOR 0
#define DRM_UAPI_VERSION_PATCH 0

/* Ioctl base — compat Linux DRM */
#define DRM_IOCTL_BASE          'd'
#define DRM_COMMAND_BASE        0x40
#define DRM_COMMAND_END         0xA0

/* Helper para _IO — userspace usa <sys/ioctl.h> mas kernel define aqui para ABI doc */
#ifndef _IO
#define _IO(type,nr)            (((type) << 8) | (nr))
#define _IOR(type,nr,size)      _IO(type,nr)
#define _IOW(type,nr,size)      _IO(type,nr)
#define _IOWR(type,nr,size)     _IO(type,nr)
#endif

/* Core ioctls (0x00-0x3F) — nunca mudar número */
#define DRM_IOCTL_VERSION       _IOR(DRM_IOCTL_BASE, 0x00, struct drm_version)
#define DRM_IOCTL_GET_MAGIC     _IOR(DRM_IOCTL_BASE, 0x02, struct drm_auth)
#define DRM_IOCTL_GEM_CREATE    _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE+0, struct drm_gem_create)
#define DRM_IOCTL_GEM_MMAP      _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE+1, struct drm_gem_mmap)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE+2, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE _IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE+3, struct drm_prime_handle)
#define DRM_IOCTL_MODE_GETRESOURCES  _IOR(DRM_IOCTL_BASE, 0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC   _IOWR(DRM_IOCTL_BASE, 0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC   _IOWR(DRM_IOCTL_BASE, 0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_ATOMIC    _IOWR(DRM_IOCTL_BASE, 0xA3, struct drm_mode_atomic)

/* Version — quebrado em uAPI 1.0 */
struct drm_version {
    int32_t version_major;
    int32_t version_minor;
    int32_t version_patchlevel;
    uint64_t name_len;
    char *name;
    uint64_t date_len;
    char *date;
    uint64_t desc_len;
    char *desc;
};

/* GEM — VRAM/GTT carveout (kernel/drivers/drm/drm_gem.c:1) */
#define GEM_DOMAIN_CPU      0
#define GEM_DOMAIN_GTT      1
#define GEM_DOMAIN_VRAM     2

struct drm_gem_create {
    uint64_t size;      /* in, bytes (page aligned) */
    uint32_t handle;    /* out, GEM handle */
    uint32_t domain;    /* in, GEM_DOMAIN_* */
    uint64_t offset;    /* out, VRAM offset if VRAM */
};

struct drm_gem_mmap {
    uint32_t handle;    /* in */
    uint32_t pad;
    uint64_t offset;    /* out, fake offset for mmap */
    uint64_t size;      /* out */
};

/* PRIME dma-buf (kernel/drivers/drm/dma_buf.c:1) */
struct drm_prime_handle {
    uint32_t handle;    /* GEM handle */
    uint32_t flags;
    int32_t fd;         /* dma-buf fd */
};

/* KMS — atomic (kernel/drivers/drm/drm_atomic.c:1) */
struct drm_mode_crtc {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t width, height;
    uint32_t mode_valid;
};

struct drm_mode_atomic {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
};

#endif /* _UAPI_DRM_H_ */
