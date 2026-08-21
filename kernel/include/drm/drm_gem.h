#ifndef _DRM_GEM_H_
#define _DRM_GEM_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <spinlock.h>

struct drm_device;

// BO memory domains
#define DRM_GEM_DOMAIN_SYSTEM 0
#define DRM_GEM_DOMAIN_GTT    1
#define DRM_GEM_DOMAIN_VRAM   2

// BO flags
#define DRM_GEM_FLAG_CACHED   (1 << 0)
#define DRM_GEM_FLAG_WC       (1 << 1)

// GEM buffer object. Refcount is protected by ref_lock (spinlock).
// refcount == 0 triggers gem_destroy() and frees all backing storage.
struct drm_gem_object {
    struct drm_device *dev;      // Owning DRM device
    size_t size;                 // Page-aligned allocation size
    uint32_t domain;             // DRM_GEM_DOMAIN_*
    uint32_t flags;              // DRM_GEM_FLAG_*
    uint32_t refcount;           // Protected by ref_lock
    spinlock_irq_t ref_lock;     // Refcount spinlock
    uint64_t phys;               // Contiguous physical base (VRAM or first GTT page)
    uint64_t *pages;             // GTT: per-page physical addresses (NULL for VRAM)
    size_t num_pages;            // Number of physical pages
    uint64_t vram_offset;        // VRAM: offset inside the carveout region
    void *vaddr;                 // Current kernel mapping (NULL when unmapped)
    bool pinned;                 // Mapped == pinned (shrinker skips pinned BOs)
    struct drm_gem_object *next; // Live-object list node
};

// Register/unregister a device with the GEM manager.
// The carveout manager owns [vram_base, vram_base + vram_size).
void drm_gem_init(struct drm_device *dev, uint64_t vram_base, size_t vram_size);
void drm_gem_fini(struct drm_device *dev);

// BO lifecycle
int  gem_create(struct drm_device *dev, size_t size, uint32_t domain,
                struct drm_gem_object **out);
void gem_destroy(struct drm_gem_object *obj);

// mmap: map the BO backing storage into kernel virtual space.
void *gem_mmap_phys(struct drm_gem_object *obj);
void  gem_unmap_phys(struct drm_gem_object *obj);

// Refcount (spinlock-protected)
void     gem_get(struct drm_gem_object *obj);
void     gem_put(struct drm_gem_object *obj);
uint32_t gem_refcount(struct drm_gem_object *obj);

// VRAM carveout accounting
size_t drm_gem_vram_used(struct drm_device *dev);
size_t drm_gem_vram_avail(struct drm_device *dev);

// Shrinker stub: eviction not implemented yet, count is informational.
size_t drm_gem_shrinker_count(void);
size_t drm_gem_shrinker_scan(void);

// Kernel selftest (dev3_test-style probe):
// creates a BO, writes, reads back, unmaps, destroys.
int drm_gem_test(void);

#endif /* _DRM_GEM_H_ */