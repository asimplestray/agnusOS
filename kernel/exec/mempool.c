#include <exec/exec.h>
#include <pmm.h>
#include <vmm.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>

/* External reference to kernel PML4 */
extern uint64_t kernel_pml4_phys;

/* Memory pool internal structure */
struct mem_pool {
    uint32_t mp_Flags;
    uint32_t mp_PudgeSize;
    uint32_t mp_ThreshSize;
    uint32_t mp_TotalSize;
    uint32_t mp_FreeSize;
    void    *mp_Memory;       /* Base address of pool memory */
    void    *mp_CurrentPtr;   /* Current allocation pointer */
    bool     mp_Reverse;      /* Allocate from high to low */
    struct mem_pool *mp_Next; /* For global pool list */
};

static mem_pool_t *global_pool_list = NULL;
static spinlock_irq_t pool_list_lock = { SPINLOCK_INIT, 0 };

/* Helper: allocate pool memory based on flags */
static void *mempool_alloc_memory(uint32_t size, uint32_t flags) {
    if (flags & MEMF_CHIP) {
        /* CHIP memory: try to use VRAM / DMA-accessible memory */
        /* For now, use regular RAM with WC attribute via VMM */
        uint32_t aligned = (size + 4095) & ~4095ULL;
        uint64_t phys = pmm_alloc_block();
        if (!phys) return NULL;
        
        uint64_t virt = (uint64_t)kmalloc(aligned);
        if (!virt) {
            pmm_free_block(phys);
            return NULL;
        }
        
        uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_WC;
        for (uint32_t off = 0; off < aligned; off += 4096) {
            vmm_map_page_in_pml4(kernel_pml4_phys, virt + off, phys + off, vmm_flags);
        }
        return (void *)virt;
    } else {
        /* FAST/PUBLIC: use kernel heap */
        return kmalloc(size);
    }
}

static void mempool_free_memory(void *ptr, uint32_t size, uint32_t flags) {
    if (!ptr) return;
    
    uint32_t aligned = (size + 4095) & ~4095ULL;
    
    if (flags & MEMF_CHIP) {
        /* Unmap and free physical pages */
        uint64_t virt = (uint64_t)ptr;
        for (uint32_t off = 0; off < aligned; off += 4096) {
            uint64_t phys = vmm_get_phys(kernel_pml4_phys, virt + off);
            if (phys) {
                vmm_unmap_page_in_pml4(kernel_pml4_phys, virt + off);
                pmm_free_block(phys);
            }
        }
        kfree(ptr); /* Free the virtual address tracking */
    } else {
        kfree(ptr);
    }
}

mem_pool_t *exec_create_pool(uint32_t flags, uint32_t pudge_size, uint32_t thresh_size) {
    if (!pudge_size) pudge_size = 4096;
    if (!thresh_size) thresh_size = 4096;
    
    mem_pool_t *pool = (mem_pool_t *)kmalloc(sizeof(mem_pool_t));
    if (!pool) return NULL;
    
    memset(pool, 0, sizeof(mem_pool_t));
    
    pool->mp_Flags = flags;
    pool->mp_PudgeSize = pudge_size;
    pool->mp_ThreshSize = thresh_size;
    pool->mp_Reverse = (flags & MEMF_REVERSE) != 0;
    
    /* Add to global list */
    unsigned long irq_flags;
    spin_lock_irqsave(&pool_list_lock, &irq_flags);
    pool->mp_Next = global_pool_list;
    global_pool_list = pool;
    spin_unlock_irqrestore(&pool_list_lock, irq_flags);
    
    serial_print("EXEC: Created memory pool flags=0x");
    char buf[16];
    itoa(flags, buf, 16);
    serial_print(buf);
    serial_print(" pudge=");
    itoa(pudge_size, buf, 10);
    serial_print(buf);
    serial_print(" thresh=");
    itoa(thresh_size, buf, 10);
    serial_print(buf);
    serial_print("\n");
    
    return pool;
}

void exec_delete_pool(mem_pool_t *pool) {
    if (!pool) return;
    
    if (pool->mp_Memory) {
        mempool_free_memory(pool->mp_Memory, pool->mp_TotalSize, pool->mp_Flags);
    }
    
    /* Remove from global list */
    unsigned long irq_flags;
    spin_lock_irqsave(&pool_list_lock, &irq_flags);
    if (global_pool_list == pool) {
        global_pool_list = pool->mp_Next;
    } else {
        mem_pool_t *p = global_pool_list;
        while (p && p->mp_Next != pool) p = p->mp_Next;
        if (p) p->mp_Next = pool->mp_Next;
    }
    spin_unlock_irqrestore(&pool_list_lock, irq_flags);
    
    kfree(pool);
}

void *exec_alloc_pooled(mem_pool_t *pool, uint32_t size) {
    if (!pool || size == 0) return NULL;
    
    uint32_t aligned = (size + 15) & ~15ULL; /* 16-byte alignment */
    
    /* Initialize pool on first allocation */
    if (!pool->mp_Memory) {
        uint32_t initial = pool->mp_PudgeSize;
        if (initial < size) initial = aligned;
        
        pool->mp_Memory = mempool_alloc_memory(initial, pool->mp_Flags);
        if (!pool->mp_Memory) return NULL;
        
        pool->mp_TotalSize = initial;
        pool->mp_FreeSize = initial;
        pool->mp_CurrentPtr = pool->mp_Reverse ? 
            (char *)pool->mp_Memory + initial : pool->mp_Memory;
    }
    
    /* Check if we have enough space */
    if (aligned > pool->mp_FreeSize) {
        /* Try to expand pool */
        uint32_t expand = pool->mp_PudgeSize;
        if (expand < aligned) expand = aligned;
        
        void *new_mem = mempool_alloc_memory(expand, pool->mp_Flags);
        if (!new_mem) return NULL;
        
        if (pool->mp_Reverse) {
            /* Prepend to pool */
            pool->mp_Memory = new_mem;
            pool->mp_CurrentPtr = (char *)new_mem + expand;
        } else {
            /* Append - for simplicity, we just track separately.
             * In a full implementation, we'd maintain a free list. */
        }
        pool->mp_TotalSize += expand;
        pool->mp_FreeSize += expand;
    }
    
    void *ptr;
    if (pool->mp_Reverse) {
        pool->mp_CurrentPtr = (char *)pool->mp_CurrentPtr - aligned;
        ptr = pool->mp_CurrentPtr;
    } else {
        ptr = pool->mp_CurrentPtr;
        pool->mp_CurrentPtr = (char *)pool->mp_CurrentPtr + aligned;
    }
    pool->mp_FreeSize -= aligned;
    
    if (pool->mp_Flags & MEMF_CLEAR) {
        memset(ptr, 0, size);
    }
    
    return ptr;
}

void exec_free_pooled(mem_pool_t *pool, void *ptr, uint32_t size) {
    if (!pool || !ptr || size == 0) return;
    
    /* This is a bump allocator - individual frees are not returned to pool.
     * Memory is reclaimed when pool is deleted.
     * For a full implementation, a free list would be maintained.
     */
    (void)pool;
    (void)ptr;
    (void)size;
}

uint32_t exec_pool_available(mem_pool_t *pool, uint32_t flags) {
    if (!pool) return 0;
    (void)flags;
    return pool->mp_FreeSize;
}

void *exec_alloc_mem(uint32_t size, uint32_t flags) {
    if (size == 0) return NULL;
    
    void *ptr = mempool_alloc_memory(size, flags);
    if (ptr && (flags & MEMF_CLEAR)) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void exec_free_mem(void *ptr, uint32_t size) {
    if (!ptr || size == 0) return;
    
    /* We can't easily determine flags from ptr alone.
     * In a real implementation, we'd track this.
     * For now, assume it's not CHIP memory. */
    kfree(ptr);
}

uint32_t exec_avail_mem(uint32_t flags) {
    (void)flags;
    return (uint32_t)(pmm_get_free_memory() / 1024);
}