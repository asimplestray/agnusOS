#include <drm/dma_resv.h>
#include <stdlib.h>
#include <string.h>
#include <kheap.h>
#include <spinlock.h>
#include <serial.h>
#include <screen.h>
#include <stdio.h>

void dma_resv_init(struct dma_resv *obj)
{
    if (!obj)
        return;
    
    spinlock_init(&obj->lock.lock);
    obj->fence = NULL;
    obj->fences = NULL;
    obj->num_fences = 0;
    obj->max_fences = 0;
    obj->seq = 0;
    obj->owner = NULL;
}

void dma_resv_fini(struct dma_resv *obj)
{
    if (!obj)
        return;
    
    dma_resv_lock(obj);
    
    if (obj->fence) {
        dma_fence_put(obj->fence);
        obj->fence = NULL;
    }
    
    for (uint32_t i = 0; i < obj->num_fences; i++) {
        if (obj->fences[i])
            dma_fence_put(obj->fences[i]);
    }
    
    if (obj->fences) {
        kfree(obj->fences);
        obj->fences = NULL;
    }
    
    obj->num_fences = 0;
    obj->max_fences = 0;
    
    dma_resv_unlock(obj);
}

void dma_resv_lock(struct dma_resv *obj)
{
    if (!obj)
        return;
    spin_lock_irqsave(&obj->lock, &obj->lock.flags);
}

void dma_resv_unlock(struct dma_resv *obj)
{
    if (!obj)
        return;
    spin_unlock_irqrestore(&obj->lock, obj->lock.flags);
}

int dma_resv_lock_interruptible(struct dma_resv *obj)
{
    if (!obj)
        return -EINVAL;
    
    spin_lock_irqsave(&obj->lock, &obj->lock.flags);
    return 0;
}

int dma_resv_trylock(struct dma_resv *obj)
{
    if (!obj)
        return -EINVAL;
    
    return spin_trylock(&obj->lock.lock) ? 0 : -EBUSY;
}

static int dma_resv_grow_fences(struct dma_resv *obj, uint32_t needed)
{
    if (obj->max_fences >= needed)
        return 0;
    
    uint32_t new_max = obj->max_fences ? obj->max_fences * 2 : 4;
    while (new_max < needed)
        new_max *= 2;
    
    if (new_max > DMA_RESV_MAX_FENCES)
        new_max = DMA_RESV_MAX_FENCES;
    
    struct dma_fence **new_fences = krealloc(obj->fences, sizeof(struct dma_fence *) * new_max);
    if (!new_fences)
        return -ENOMEM;
    
    memset(new_fences + obj->num_fences, 0, sizeof(struct dma_fence *) * (new_max - obj->num_fences));
    
    obj->fences = new_fences;
    obj->max_fences = new_max;
    
    return 0;
}

int dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence, unsigned int usage)
{
    if (!obj || !fence)
        return -EINVAL;
    
    if (usage & DMA_RESV_USAGE_WRITE)
        return dma_resv_add_excl_fence(obj, fence);
    else if (usage & DMA_RESV_USAGE_READ)
        return dma_resv_add_shared_fence(obj, fence);
    
    return -EINVAL;
}

int dma_resv_add_excl_fence(struct dma_resv *obj, struct dma_fence *fence)
{
    if (!obj || !fence)
        return -EINVAL;
    
    dma_resv_lock(obj);
    
    if (obj->fence)
        dma_fence_put(obj->fence);
    
    obj->fence = dma_fence_get(fence);
    obj->seq++;
    
    dma_resv_unlock(obj);
    
    return 0;
}

int dma_resv_add_shared_fence(struct dma_resv *obj, struct dma_fence *fence)
{
    if (!obj || !fence)
        return -EINVAL;
    
    dma_resv_lock(obj);
    
    int ret = dma_resv_grow_fences(obj, obj->num_fences + 1);
    if (ret) {
        dma_resv_unlock(obj);
        return ret;
    }
    
    obj->fences[obj->num_fences] = dma_fence_get(fence);
    obj->num_fences++;
    obj->seq++;
    
    dma_resv_unlock(obj);
    
    return 0;
}

void dma_resv_replace_fences(struct dma_resv *obj, struct dma_fence *fence)
{
    if (!obj)
        return;
    
    dma_resv_lock(obj);
    
    if (obj->fence)
        dma_fence_put(obj->fence);
    
    obj->fence = fence ? dma_fence_get(fence) : NULL;
    
    for (uint32_t i = 0; i < obj->num_fences; i++) {
        if (obj->fences[i])
            dma_fence_put(obj->fences[i]);
    }
    
    obj->num_fences = 0;
    obj->seq++;
    
    dma_resv_unlock(obj);
}

int dma_resv_wait_timeout(struct dma_resv *obj, bool intr, int64_t timeout_ns, unsigned int usage)
{
    if (!obj)
        return -EINVAL;
    
    dma_resv_lock(obj);
    
    int ret = 0;
    
    if (usage & DMA_RESV_USAGE_WRITE) {
        if (obj->fence) {
            struct dma_fence *fence = dma_fence_get(obj->fence);
            dma_resv_unlock(obj);
            ret = dma_fence_wait_timeout(fence, intr, timeout_ns);
            dma_fence_put(fence);
        } else {
            dma_resv_unlock(obj);
        }
    }
    
    if ((usage & DMA_RESV_USAGE_READ) && ret == 0) {
        struct dma_fence **fences = NULL;
        uint32_t count = 0;
        
        if (obj->num_fences > 0) {
            fences = kmalloc(sizeof(struct dma_fence *) * obj->num_fences);
            if (fences) {
                for (uint32_t i = 0; i < obj->num_fences; i++) {
                    fences[i] = dma_fence_get(obj->fences[i]);
                }
                count = obj->num_fences;
            }
        }
        
        dma_resv_unlock(obj);
        
        if (count > 0) {
            ret = dma_fence_wait_any_timeout(fences, count, intr, timeout_ns, NULL);
            
            for (uint32_t i = 0; i < count; i++)
                dma_fence_put(fences[i]);
            
            kfree(fences);
        }
    } else if (ret == 0) {
        dma_resv_unlock(obj);
    }
    
    return ret;
}

struct dma_fence *dma_resv_get_excl(struct dma_resv *obj)
{
    if (!obj)
        return NULL;
    
    dma_resv_lock(obj);
    struct dma_fence *fence = obj->fence ? dma_fence_get(obj->fence) : NULL;
    dma_resv_unlock(obj);
    
    return fence;
}

struct dma_fence **dma_resv_get_shared(struct dma_resv *obj, uint32_t *count)
{
    if (!obj || !count)
        return NULL;
    
    dma_resv_lock(obj);
    
    struct dma_fence **fences = NULL;
    
    if (obj->num_fences > 0) {
        fences = kmalloc(sizeof(struct dma_fence *) * obj->num_fences);
        if (fences) {
            for (uint32_t i = 0; i < obj->num_fences; i++) {
                fences[i] = obj->fences[i] ? dma_fence_get(obj->fences[i]) : NULL;
            }
            *count = obj->num_fences;
        } else {
            *count = 0;
        }
    } else {
        *count = 0;
    }
    
    dma_resv_unlock(obj);
    
    return fences;
}

int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src)
{
    if (!dst || !src)
        return -EINVAL;
    
    dma_resv_lock(src);
    dma_resv_lock(dst);
    
    if (dst->fence)
        dma_fence_put(dst->fence);
    
    dst->fence = src->fence ? dma_fence_get(src->fence) : NULL;
    
    int ret = dma_resv_grow_fences(dst, src->num_fences);
    if (ret) {
        dma_resv_unlock(dst);
        dma_resv_unlock(src);
        return ret;
    }
    
    for (uint32_t i = 0; i < src->num_fences; i++) {
        if (dst->fences[i])
            dma_fence_put(dst->fences[i]);
        dst->fences[i] = src->fences[i] ? dma_fence_get(src->fences[i]) : NULL;
    }
    
    dst->num_fences = src->num_fences;
    dst->seq = src->seq;
    
    dma_resv_unlock(dst);
    dma_resv_unlock(src);
    
    return 0;
}

bool dma_resv_test_signaled(struct dma_resv *obj, unsigned int usage)
{
    if (!obj)
        return true;
    
    dma_resv_lock(obj);
    
    bool signaled = true;
    
    if (usage & DMA_RESV_USAGE_WRITE) {
        if (obj->fence && !dma_fence_is_signaled(obj->fence))
            signaled = false;
    }
    
    if (signaled && (usage & DMA_RESV_USAGE_READ)) {
        for (uint32_t i = 0; i < obj->num_fences; i++) {
            if (obj->fences[i] && !dma_fence_is_signaled(obj->fences[i])) {
                signaled = false;
                break;
            }
        }
    }
    
    dma_resv_unlock(obj);
    
    return signaled;
}

void dma_resv_debug_dump(struct dma_resv *obj, const char *prefix)
{
    if (!obj)
        return;
    
    dma_resv_lock(obj);
    
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: dma_resv dump", prefix ? prefix : "dma_resv");
    screen_log("DEBUG", COLOR_CYAN, buf);
    
    snprintf(buf, sizeof(buf), "  seq: %llu", (unsigned long long)obj->seq);
    screen_log("DEBUG", COLOR_CYAN, buf);
    
    snprintf(buf, sizeof(buf), "  excl fence: %p", obj->fence);
    screen_log("DEBUG", COLOR_CYAN, buf);
    
    if (obj->fence) {
        snprintf(buf, sizeof(buf), "    context: %llu, seqno: %llu, signaled: %d",
                 (unsigned long long)obj->fence->context,
                 (unsigned long long)obj->fence->seqno,
                 dma_fence_is_signaled(obj->fence));
        screen_log("DEBUG", COLOR_CYAN, buf);
    }
    
    snprintf(buf, sizeof(buf), "  shared fences: %u", obj->num_fences);
    screen_log("DEBUG", COLOR_CYAN, buf);
    
    for (uint32_t i = 0; i < obj->num_fences; i++) {
        if (obj->fences[i]) {
            snprintf(buf, sizeof(buf), "    [%u]: %p context: %llu, seqno: %llu, signaled: %d",
                     i, obj->fences[i],
                     (unsigned long long)obj->fences[i]->context,
                     (unsigned long long)obj->fences[i]->seqno,
                     dma_fence_is_signaled(obj->fences[i]));
            screen_log("DEBUG", COLOR_CYAN, buf);
        }
    }
    
    dma_resv_unlock(obj);
}