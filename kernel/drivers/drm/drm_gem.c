// DRM GEM (Graphics Execution Manager) - BO create/destroy/mmap/refcount,
// VRAM carveout, shrinker stub.
//
// Phase 1 - Dev 2 deliverable.

#include <drm/drm_gem.h>
#include <drm/drm_device.h>
#include <pmm.h>
#include <vmm.h>
#include <kheap.h>
#include <string.h>
#include <screen.h>
#include <serial.h>

#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOSPC
#define ENOSPC 28
#endif

#define DRM_GEM_MAX_DEVICES 16
#define DRM_GEM_MMAP_BASE   0xFFFF800400000000ULL
#define DRM_GEM_MMAP_SIZE   (1ULL << 30)

// Free range inside the VRAM carveout (kept sorted by offset)
struct gem_carveout_block {
    uint64_t offset;
    size_t size;
    struct gem_carveout_block *next;
};

struct drm_gem_device {
    struct drm_device *dev;          // NULL == free slot
    uint64_t vram_base;
    size_t vram_size;
    size_t vram_used;
    struct gem_carveout_block *carveout; // sorted free list
    struct drm_gem_object *objects;      // live objects (shrinker list)
    spinlock_irq_t lock;
    uint64_t mmap_next;                  // next free mmap window
};

static struct drm_gem_device gem_devices[DRM_GEM_MAX_DEVICES];

static struct drm_gem_device *gem_dev_for(const struct drm_device *dev)
{
    for (int i = 0; i < DRM_GEM_MAX_DEVICES; i++) {
        if (gem_devices[i].dev == dev)
            return &gem_devices[i];
    }
    return NULL;
}

static struct drm_gem_device *gem_slot_alloc(const struct drm_device *dev)
{
    for (int i = 0; i < DRM_GEM_MAX_DEVICES; i++) {
        if (gem_devices[i].dev == NULL) {
            memset(&gem_devices[i], 0, sizeof(gem_devices[i]));
            gem_devices[i].dev = (struct drm_device *)dev;
            spinlock_init(&gem_devices[i].lock.lock);
            return &gem_devices[i];
        }
    }
    return NULL;
}

static void gem_serial_hex(uint64_t v)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[19];
    int shift = 60;
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[i + 2] = hexd[(v >> shift) & 0xF];
        shift -= 4;
    }
    buf[18] = '\0';
    serial_print(buf);
}

static void gem_serial_dec(size_t v)
{
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    if (v == 0)
        buf[--i] = '0';
    while (v > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    serial_print(&buf[i]);
}

// ---------------------------------------------------------------------------
// VRAM carveout (first-fit with split; free with coalescing)
// ---------------------------------------------------------------------------

static int carve_alloc(struct drm_gem_device *g, size_t size, uint64_t *offset)
{
    struct gem_carveout_block **p = &g->carveout;

    while (*p) {
        struct gem_carveout_block *b = *p;
        if (b->size >= size) {
            *offset = b->offset;
            g->vram_used += size;
            if (b->size == size) {
                *p = b->next;
                kfree(b);
            } else {
                b->offset += size;
                b->size -= size;
            }
            return 0;
        }
        p = &b->next;
    }
    return -ENOSPC;
}

static void carve_free(struct drm_gem_device *g, uint64_t offset, size_t size)
{
    struct gem_carveout_block *nb = kmalloc(sizeof(struct gem_carveout_block));
    if (!nb) {
        screen_log("ERROR", COLOR_RED, "DRM/GEM: carveout metadata allocation failed");
        return;
    }
    nb->offset = offset;
    nb->size = size;
    g->vram_used -= size;

    struct gem_carveout_block **p = &g->carveout;
    while (*p && (*p)->offset < offset)
        p = &(*p)->next;
    nb->next = *p;
    *p = nb;

    // Coalesce with previous and/or next neighbour
    struct gem_carveout_block *cur = g->carveout;
    while (cur && cur->next) {
        if (cur->offset + cur->size == cur->next->offset) {
            cur->size += cur->next->size;
            struct gem_carveout_block *dead = cur->next;
            cur->next = dead->next;
            kfree(dead);
            continue;
        }
        cur = cur->next;
    }
}

// ---------------------------------------------------------------------------
// Object management
// ---------------------------------------------------------------------------

static void gem_list_add(struct drm_gem_device *g, struct drm_gem_object *obj)
{
    obj->next = g->objects;
    g->objects = obj;
}

static void gem_list_del(struct drm_gem_device *g, struct drm_gem_object *obj)
{
    struct drm_gem_object **p = &g->objects;
    while (*p) {
        if (*p == obj) {
            *p = obj->next;
            return;
        }
        p = &(*p)->next;
    }
}

static void gem_unmap_locked(struct drm_gem_object *obj)
{
    if (!obj->vaddr)
        return;
    uint64_t win = (uint64_t)obj->vaddr;
    for (size_t i = 0; i < obj->num_pages; i++)
        vmm_unmap_page(win + i * PAGE_SIZE);
    obj->vaddr = NULL;
    obj->pinned = false;
}

// Must be called with the device lock held; refcount must be 0.
static void gem_free_locked(struct drm_gem_device *g, struct drm_gem_object *obj)
{
    if (obj->vaddr)
        gem_unmap_locked(obj);

    if (obj->domain == DRM_GEM_DOMAIN_VRAM)
        carve_free(g, obj->vram_offset, obj->size);
    else {
        for (size_t i = 0; i < obj->num_pages; i++) {
            if (obj->pages[i])
                pmm_free_block(obj->pages[i]);
        }
    }
    if (obj->pages)
        kfree(obj->pages);

    gem_list_del(g, obj);
    kfree(obj);
}

void drm_gem_init(struct drm_device *dev, uint64_t vram_base, size_t vram_size)
{
    if (gem_dev_for(dev)) {
        screen_log("WARN", COLOR_BROWN, "DRM/GEM: device already registered");
        return;
    }

    struct drm_gem_device *g = gem_slot_alloc(dev);
    if (!g) {
        screen_log("ERROR", COLOR_RED, "DRM/GEM: no free device slot");
        return;
    }

    vram_size &= ~(PAGE_SIZE - 1);
    g->vram_base = vram_base;
    g->vram_size = vram_size;
    g->mmap_next = DRM_GEM_MMAP_BASE;

    if (vram_size > 0) {
        struct gem_carveout_block *head = kmalloc(sizeof(struct gem_carveout_block));
        if (!head) {
            g->dev = NULL;
            screen_log("ERROR", COLOR_RED, "DRM/GEM: carveout init allocation failed");
            return;
        }
        head->offset = 0;
        head->size = vram_size;
        head->next = NULL;
        g->carveout = head;
    }

    serial_print("DRM/GEM: device registered vram=");
    gem_serial_hex(g->vram_base);
    serial_print(" size=");
    gem_serial_dec(g->vram_size);
    serial_print("\n");
}

static void gem_slot_clear(struct drm_gem_device *g)
{
    while (g->objects) {
        struct drm_gem_object *obj = g->objects;
        gem_free_locked(g, obj);
    }

    while (g->carveout) {
        struct gem_carveout_block *dead = g->carveout;
        g->carveout = dead->next;
        kfree(dead);
    }
    memset(g, 0, sizeof(*g));
}

void drm_gem_fini(struct drm_device *dev)
{
    struct drm_gem_device *g = gem_dev_for(dev);
    if (!g)
        return;
    serial_print("DRM/GEM: device unregistered\n");
    gem_slot_clear(g);
}

int gem_create(struct drm_device *dev, size_t size, uint32_t domain,
               struct drm_gem_object **out)
{
    if (!dev || !out)
        return -EINVAL;

    struct drm_gem_device *g = gem_dev_for(dev);
    if (!g)
        return -EINVAL;

    if (domain != DRM_GEM_DOMAIN_VRAM && domain != DRM_GEM_DOMAIN_GTT)
        return -EINVAL;

    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (size == 0)
        return -EINVAL;

    struct drm_gem_object *obj = kmalloc(sizeof(struct drm_gem_object));
    if (!obj)
        return -ENOMEM;
    memset(obj, 0, sizeof(*obj));

    obj->dev = dev;
    obj->size = size;
    obj->domain = domain;
    obj->refcount = 1;
    obj->num_pages = size / PAGE_SIZE;
    spinlock_init(&obj->ref_lock.lock);

    unsigned long flags;
    spin_lock_irqsave(&g->lock, &flags);

    if (domain == DRM_GEM_DOMAIN_VRAM) {
        uint64_t off;
        if (carve_alloc(g, size, &off) != 0) {
            spin_unlock_irqrestore(&g->lock, flags);
            kfree(obj);
            return -ENOSPC;
        }
        obj->vram_offset = off;
        obj->phys = g->vram_base + off;
        gem_list_add(g, obj);
    } else {
        obj->pages = kmalloc(obj->num_pages * sizeof(uint64_t));
        if (!obj->pages) {
            spin_unlock_irqrestore(&g->lock, flags);
            kfree(obj);
            return -ENOMEM;
        }
        memset(obj->pages, 0, obj->num_pages * sizeof(uint64_t));
        for (size_t i = 0; i < obj->num_pages; i++) {
            uint64_t pg = pmm_alloc_block();
            if (!pg) {
                for (size_t j = 0; j < i; j++)
                    pmm_free_block(obj->pages[j]);
                kfree(obj->pages);
                spin_unlock_irqrestore(&g->lock, flags);
                kfree(obj);
                return -ENOMEM;
            }
            obj->pages[i] = pg;
        }
        obj->phys = obj->pages[0];
        gem_list_add(g, obj);
    }

    spin_unlock_irqrestore(&g->lock, flags);

    *out = obj;
    serial_print("DRM/GEM: BO created size=");
    gem_serial_dec(size);
    serial_print(" domain=");
    gem_serial_dec(domain);
    serial_print(" phys=");
    gem_serial_hex(obj->phys);
    serial_print("\n");
    return 0;
}

void gem_destroy(struct drm_gem_object *obj)
{
    if (!obj)
        return;
    struct drm_gem_device *g = gem_dev_for(obj->dev);
    if (!g)
        return;

    unsigned long flags;
    spin_lock_irqsave(&g->lock, &flags);
    gem_free_locked(g, obj);
    spin_unlock_irqrestore(&g->lock, flags);
}

void *gem_mmap_phys(struct drm_gem_object *obj)
{
    if (!obj)
        return NULL;

    struct drm_gem_device *g = gem_dev_for(obj->dev);
    if (!g)
        return NULL;

    unsigned long flags;
    spin_lock_irqsave(&g->lock, &flags);

    if (obj->vaddr) {
        spin_unlock_irqrestore(&g->lock, flags);
        return obj->vaddr;
    }

    if (g->mmap_next + obj->size > DRM_GEM_MMAP_BASE + DRM_GEM_MMAP_SIZE) {
        spin_unlock_irqrestore(&g->lock, flags);
        screen_log("ERROR", COLOR_RED, "DRM/GEM: mmap window exhausted");
        return NULL;
    }

    uint64_t win = g->mmap_next;
    g->mmap_next += obj->size;

    if (obj->domain == DRM_GEM_DOMAIN_VRAM) {
        vmm_map_region(win, obj->phys, (uint32_t)obj->size,
                       VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    } else {
        for (size_t i = 0; i < obj->num_pages; i++) {
            vmm_map_page(win + i * PAGE_SIZE, obj->pages[i],
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        }
    }

    obj->vaddr = (void *)win;
    obj->pinned = true;
    spin_unlock_irqrestore(&g->lock, flags);

    serial_print("DRM/GEM: BO mapped at ");
    gem_serial_hex(win);
    serial_print("\n");
    return obj->vaddr;
}

void gem_unmap_phys(struct drm_gem_object *obj)
{
    if (!obj)
        return;

    struct drm_gem_device *g = gem_dev_for(obj->dev);
    if (!g)
        return;

    unsigned long flags;
    spin_lock_irqsave(&g->lock, &flags);
    bool was_mapped = obj->vaddr != NULL;
    gem_unmap_locked(obj);
    spin_unlock_irqrestore(&g->lock, flags);

    if (was_mapped)
        serial_print("DRM/GEM: BO unmapped\n");
}

void gem_get(struct drm_gem_object *obj)
{
    if (!obj)
        return;
    unsigned long flags;
    spin_lock_irqsave(&obj->ref_lock, &flags);
    obj->refcount++;
    spin_unlock_irqrestore(&obj->ref_lock, flags);
}

void gem_put(struct drm_gem_object *obj)
{
    if (!obj)
        return;
    unsigned long flags;
    spin_lock_irqsave(&obj->ref_lock, &flags);
    obj->refcount--;
    uint32_t n = obj->refcount;
    spin_unlock_irqrestore(&obj->ref_lock, flags);

    if (n == 0)
        gem_destroy(obj);
}

uint32_t gem_refcount(struct drm_gem_object *obj)
{
    if (!obj)
        return 0;
    unsigned long flags;
    spin_lock_irqsave(&obj->ref_lock, &flags);
    uint32_t n = obj->refcount;
    spin_unlock_irqrestore(&obj->ref_lock, flags);
    return n;
}

size_t drm_gem_vram_used(struct drm_device *dev)
{
    struct drm_gem_device *g = gem_dev_for(dev);
    if (!g)
        return 0;
    return g->vram_used;
}

size_t drm_gem_vram_avail(struct drm_device *dev)
{
    struct drm_gem_device *g = gem_dev_for(dev);
    if (!g)
        return 0;
    return g->vram_size - g->vram_used;
}

// ---------------------------------------------------------------------------
// Shrinker stub: count reclaimable (unpinned, non-VRAM) memory.
// scan() does nothing yet - eviction comes with the fence/eviction work.
// ---------------------------------------------------------------------------

static size_t gem_shrinker_total_unpinned(size_t *bo_count)
{
    size_t total = 0;
    size_t n = 0;
    for (int i = 0; i < DRM_GEM_MAX_DEVICES; i++) {
        struct drm_gem_device *g = &gem_devices[i];
        if (!g->dev)
            continue;
        for (struct drm_gem_object *obj = g->objects; obj; obj = obj->next) {
            if (obj->domain == DRM_GEM_DOMAIN_GTT && !obj->pinned) {
                total += obj->size;
                n++;
            }
        }
    }
    if (bo_count)
        *bo_count = n;
    return total;
}

size_t drm_gem_shrinker_count(void)
{
    return gem_shrinker_total_unpinned(NULL);
}

size_t drm_gem_shrinker_scan(void)
{
    // Stub: no eviction implemented yet. Managed eviction is a later phase.
    return 0;
}

// ---------------------------------------------------------------------------
// Kernel selftest (dev3_test-style probe):
// fake VRAM backed by system pages, then create/write/read/unmap/destroy.
// ---------------------------------------------------------------------------

#define GEM_TEST_VRAM_PAGES 256
#define GEM_TEST_VRAM_SIZE  (GEM_TEST_VRAM_PAGES * PAGE_SIZE)

static void gem_test_label(const char *name)
{
    serial_print("DRM/GEM-TEST: ");
    serial_print(name);
    serial_print("\n");
}

static int gem_test_alloc_fake_vram(uint64_t *base_out)
{
    // Claim a wider descending-free window (pmm hands out ascending pages)
    // and slide over it to find a run of GEM_TEST_VRAM_PAGES consecutive
    // pages. pmm may contain reserved holes (ACPI/e820), so gaps are real.
    enum { CLAIM = GEM_TEST_VRAM_PAGES * 4 };
    uint64_t *pages = kmalloc(CLAIM * sizeof(uint64_t));
    if (!pages)
        return -ENOMEM;

    int got = 0;
    for (int i = 0; i < CLAIM; i++) {
        pages[i] = pmm_alloc_block();
        if (!pages[i]) {
            for (int j = 0; j < i; j++)
                pmm_free_block(pages[j]);
            kfree(pages);
            return -ENOMEM;
        }
        got++;
    }

    for (int i = 0; i + GEM_TEST_VRAM_PAGES <= got; i++) {
        int ok = 1;
        for (int j = 1; j < GEM_TEST_VRAM_PAGES; j++) {
            if (pages[i + j] != pages[i] + j * PAGE_SIZE) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            *base_out = pages[i];
            for (int j = 0; j < got; j++) {
                if (j < i || j >= i + GEM_TEST_VRAM_PAGES)
                    pmm_free_block(pages[j]);
            }
            kfree(pages);
            return 0;
        }
    }

    for (int j = 0; j < got; j++)
        pmm_free_block(pages[j]);
    kfree(pages);
    return -ENOSPC;
}

static void gem_test_free_fake_vram(uint64_t base)
{
    for (int i = 0; i < GEM_TEST_VRAM_PAGES; i++)
        pmm_free_block(base + i * PAGE_SIZE);
}

int drm_gem_test(void)
{
    struct drm_device fake;
    uint64_t vram_base = 0;
    int rc;

    memset(&fake, 0, sizeof(fake));

    rc = gem_test_alloc_fake_vram(&vram_base);
    if (rc) {
        screen_log("FAIL", COLOR_LIGHT_RED, "DRM/GEM: selftest fake VRAM alloc failed");
        serial_print("DRM/GEM-TEST: fake VRAM alloc failed rc=");
        gem_serial_dec((size_t)rc);
        serial_print("\n");
        return rc;
    }
    serial_print("DRM/GEM-TEST: fake VRAM 1MB at ");
    gem_serial_hex(vram_base);
    serial_print("\n");
    drm_gem_init(&fake, vram_base, GEM_TEST_VRAM_SIZE);

    // A) GTT BO: create, mmap, write pattern, read back, unmap, destroy
    {
        struct drm_gem_object *bo = NULL;
        gem_test_label("A) GTT BO create/mmap/write/read");
        rc = gem_create(&fake, 96 * 1024, DRM_GEM_DOMAIN_GTT, &bo);
        if (rc)
            goto out;
        if (drm_gem_shrinker_count() != 96 * 1024) {
            rc = -EINVAL;
            goto out;
        }
        void *v = gem_mmap_phys(bo);
        if (!v) {
            rc = -EINVAL;
            goto out;
        }
        if (drm_gem_shrinker_count() != 0) { // pinned == not reclaimable
            rc = -EINVAL;
            goto out;
        }
        memset(v, 0xA5, 96 * 1024);
        for (int i = 0; i < 96 * 1024; i++) {
            if (((uint8_t *)v)[i] != 0xA5) {
                rc = -EINVAL;
                goto out;
            }
        }
        gem_unmap_phys(bo);
        if (drm_gem_shrinker_count() != 96 * 1024) {
            rc = -EINVAL;
            goto out;
        }
        gem_put(bo);
        if (drm_gem_shrinker_count() != 0) {
            rc = -EINVAL;
            goto out;
        }
        gem_test_label("A) OK");
    }

    // B) VRAM carveout: create, mmap, word pattern, read back, unmap, destroy
    {
        struct drm_gem_object *bo = NULL;
        gem_test_label("B) VRAM carveout create/mmap/write/read");
        rc = gem_create(&fake, 256 * 1024, DRM_GEM_DOMAIN_VRAM, &bo);
        if (rc)
            goto out;
        if (drm_gem_vram_used(&fake) != 256 * 1024) {
            rc = -EINVAL;
            goto out;
        }
        void *v = gem_mmap_phys(bo);
        if (!v) {
            rc = -EINVAL;
            goto out;
        }
        uint32_t *words = (uint32_t *)v;
        for (int i = 0; i < (256 * 1024) / 4; i++)
            words[i] = 0x11223344u + (uint32_t)i;
        for (int i = 0; i < (256 * 1024) / 4; i++) {
            if (words[i] != 0x11223344u + (uint32_t)i) {
                rc = -EINVAL;
                goto out;
            }
        }
        gem_unmap_phys(bo);
        gem_put(bo);
        if (drm_gem_vram_used(&fake) != 0) {
            rc = -EINVAL;
            goto out;
        }
        gem_test_label("B) OK");
    }

    // C) Refcount: get x2 then put until destroy
    {
        struct drm_gem_object *bo = NULL;
        gem_test_label("C) refcount get/put (spinlock)");
        rc = gem_create(&fake, 64 * 1024, DRM_GEM_DOMAIN_VRAM, &bo);
        if (rc)
            goto out;
        gem_get(bo);
        gem_get(bo);
        if (gem_refcount(bo) != 3) {
            rc = -EINVAL;
            goto out;
        }
        gem_put(bo); // 2
        gem_put(bo); // 1
        if (gem_refcount(bo) != 1) {
            rc = -EINVAL;
            goto out;
        }
        gem_put(bo); // 0 -> destroy
        if (drm_gem_vram_used(&fake) != 0) {
            rc = -EINVAL;
            goto out;
        }
        gem_test_label("C) OK");
    }

    // D) Carveout fragmentation: 3 x 128KB, free middle, re-allocate into gap
    {
        struct drm_gem_object *a = NULL, *b = NULL, *c = NULL, *d = NULL;
        gem_test_label("D) carveout fragmentation/reuse");
        rc = gem_create(&fake, 128 * 1024, DRM_GEM_DOMAIN_VRAM, &a);
        if (rc)
            goto out;
        rc = gem_create(&fake, 128 * 1024, DRM_GEM_DOMAIN_VRAM, &b);
        if (rc)
            goto out;
        rc = gem_create(&fake, 128 * 1024, DRM_GEM_DOMAIN_VRAM, &c);
        if (rc)
            goto out;
        if (drm_gem_vram_used(&fake) != 384 * 1024) {
            rc = -EINVAL;
            goto out;
        }
        gem_put(b); // free the middle range
        if (drm_gem_vram_used(&fake) != 256 * 1024) {
            rc = -EINVAL;
            goto out;
        }
        rc = gem_create(&fake, 128 * 1024, DRM_GEM_DOMAIN_VRAM, &d);
        if (rc)
            goto out;
        if (drm_gem_vram_used(&fake) != 384 * 1024) {
            rc = -EINVAL;
            goto out;
        }
        gem_put(a);
        gem_put(c);
        gem_put(d);
        if (drm_gem_vram_used(&fake) != 0) {
            rc = -EINVAL;
            goto out;
        }
        gem_test_label("D) OK");
    }

    gem_test_label("PASS - BO create/write/read/unmap OK");
    rc = 0;

out:
    drm_gem_fini(&fake);
    gem_test_free_fake_vram(vram_base);
    if (rc == 0) {
        screen_log("OK", COLOR_LIGHT_GREEN, "DRM/GEM: selftest PASS (create/write/read/unmap OK)");
    } else {
        screen_log("FAIL", COLOR_LIGHT_RED, "DRM/GEM: selftest FAILED");
        serial_print("DRM/GEM-TEST: FAILED rc=");
        gem_serial_dec((size_t)rc);
        serial_print("\n");
    }
    return rc;
}