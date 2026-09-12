#include <kheap.h>
#include <vmm.h>
#include <pmm.h>
#include <screen.h>
#include <string.h>
#include <spinlock.h>

#define HEAP_START        0x10000000 // 256MB - within boot identity-mapped range
#define HEAP_INITIAL_SIZE 0x800000   // 8MB Initial Heap Size
#define HEAP_MAX_SIZE     0x4000000  // 64MB cap for PMM-backed expansion

#define KHEAP_MAGIC_ALLOC 0x48454150u /* 'HEAP' */
#define KHEAP_MAGIC_FREE  0x46524545u /* 'FREE' */
#define KHEAP_POISON_FREE 0xAA

struct heap_block {
    uint32_t magic;
    size_t size;
    int is_free;
    struct heap_block* next;
} __attribute__((aligned(16)));

static struct heap_block* heap_start_block = 0;
static uint64_t heap_end = 0;
static spinlock_irq_t heap_lock = { SPINLOCK_INIT, 0 };

/* Observability counters. */
static uint64_t st_allocs = 0, st_frees = 0, st_fails = 0, st_bad = 0;
static uint64_t st_used = 0, st_high = 0;

/* Walk must run with heap_lock held. Validates exact block start,
 * catching interior pointers that header-arithmetic alone would miss. */
static struct heap_block *find_block_locked(void *ptr) {
    uintptr_t target = (uintptr_t)ptr;
    for (struct heap_block *b = heap_start_block; b; b = b->next) {
        void *payload = (void *)((uintptr_t)b + sizeof(struct heap_block));
        if ((uintptr_t)payload == target) return b;
        /* Bounds check: a corrupt next pointer must not loop forever. */
        if ((uintptr_t)b->next <= (uintptr_t)b &&
            b->next != 0)
            break;
    }
    return 0;
}

/* Coalesce: Merge consecutive free blocks to prevent fragmentation.
 * Lock must be held. */
static void kheap_coalesce(void) {
    struct heap_block* current = heap_start_block;
    while (current != 0) {
        if (current->is_free && current->next != 0 && current->next->is_free) {
            /* Only merge adjacent blocks (expansion appends contiguously). */
            uintptr_t expect = (uintptr_t)current + sizeof(struct heap_block) + current->size;
            if ((uintptr_t)current->next != expect) break;
            current->size = current->size + sizeof(struct heap_block) + current->next->size;
            current->next = current->next->next;
            /* Repeat on the same block in case the next-next block is also free */
            continue;
        }
        current = current->next;
    }
}

/* Try to grow the heap by mapping fresh PMM pages right after heap_end.
 * Lock must be held. Returns 1 if at least one page was added. */
static int kheap_try_expand_locked(size_t need) {
    if (heap_end >= HEAP_START + HEAP_MAX_SIZE) return 0;
    size_t want = (need + sizeof(struct heap_block) + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    if (want < PAGE_SIZE) want = PAGE_SIZE;
    uint64_t cap = HEAP_START + HEAP_MAX_SIZE;
    if (heap_end + want > cap) want = cap - heap_end;
    want &= ~(size_t)(PAGE_SIZE - 1);
    if (!want) return 0;

    uint64_t base = heap_end;
    size_t mapped = 0;
    for (size_t off = 0; off < want; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_block();
        if (!phys) break;
        /* Heap pages are supervisor-only by construction (no USER flag).
         * Every PML4 shares the kernel half, so mapping into the active
         * tables is visible everywhere. */
        vmm_map_page(base + off, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
        if (vmm_get_phys(vmm_active_pml4(), base + off) != phys) {
            pmm_free_block(phys);
            break;
        }
        mapped += PAGE_SIZE;
    }
    if (!mapped) return 0;

    struct heap_block *nb = (struct heap_block *)base;
    nb->magic = KHEAP_MAGIC_FREE;
    nb->size = mapped - sizeof(struct heap_block);
    nb->is_free = 1;
    nb->next = 0;
    /* Append at the tail (expansion is always contiguous). */
    struct heap_block *tail = heap_start_block;
    while (tail && tail->next) tail = tail->next;
    if (tail) tail->next = nb;
    else heap_start_block = nb;
    heap_end = base + mapped;
    kheap_coalesce();
    return 1;
}

void kheap_init(void) {
    spinlock_init(&heap_lock.lock);
    // The kernel heap region (256MB-256MB+8MB) is already identity-mapped with 2MB huge pages
    // by the boot page tables. We just need to initialize the heap metadata.

    // 1. Initialize the first big block spanning the entire heap
    heap_start_block = (struct heap_block*)HEAP_START;
    heap_start_block->magic = KHEAP_MAGIC_FREE;
    heap_start_block->size = HEAP_INITIAL_SIZE - sizeof(struct heap_block);
    heap_start_block->is_free = 1;
    heap_start_block->next = 0;

    heap_end = HEAP_START + HEAP_INITIAL_SIZE;

    screen_log(" OK ", COLOR_LIGHT_GREEN, "Kernel Heap Allocator (kmalloc/kfree) initialized (Size: 8MB).");
}

static void *kmalloc_locked(size_t size) {
    struct heap_block* current = heap_start_block;

    // First-Fit search
    while (current != 0) {
        if (current->is_free && current->magic == KHEAP_MAGIC_FREE &&
            current->size >= size) {
            // Can we split this block?
            if (current->size >= size + sizeof(struct heap_block) + 16) {
                struct heap_block* new_block = (struct heap_block*)((uintptr_t)current + sizeof(struct heap_block) + size);

                new_block->magic = KHEAP_MAGIC_FREE;
                new_block->size = current->size - size - sizeof(struct heap_block);
                new_block->is_free = 1;
                new_block->next = current->next;

                current->size = size;
                current->next = new_block;
            }

            current->is_free = 0;
            current->magic = KHEAP_MAGIC_ALLOC;
            st_allocs++;
            st_used += current->size;
            if (st_used > st_high) st_high = st_used;
            return (void*)((uintptr_t)current + sizeof(struct heap_block));
        }
        current = current->next;
    }
    return 0;
}

void* kmalloc(size_t size) {
    unsigned long flags;

    if (size == 0) return 0;
    if (size > SIZE_MAX - 15) { __sync_fetch_and_add(&st_fails, 1); return 0; }

    // Align to 16 bytes for fxsave (task_struct) and general 16B
    size = (size + 15) & ~15;
    spin_lock_irqsave(&heap_lock, &flags);

    void *r = kmalloc_locked(size);
    if (!r && kheap_try_expand_locked(size))
        r = kmalloc_locked(size);
    if (!r) __sync_fetch_and_add(&st_fails, 1);
    spin_unlock_irqrestore(&heap_lock, flags);
    return r;
}

void* kmalloc_array(size_t n, size_t size) {
    if (n && size > SIZE_MAX / n) { __sync_fetch_and_add(&st_fails, 1); return 0; }
    return kmalloc(n * size);
}

void* kcalloc(size_t n, size_t size) {
    if (n && size > SIZE_MAX / n) { __sync_fetch_and_add(&st_fails, 1); return 0; }
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void kfree(void* ptr) {
    unsigned long flags;

    if (!ptr) return;
    if ((uintptr_t)ptr < HEAP_START + sizeof(struct heap_block) ||
        (uintptr_t)ptr >= heap_end) {
        __sync_fetch_and_add(&st_bad, 1);
        return;
    }

    spin_lock_irqsave(&heap_lock, &flags);

    struct heap_block* block = find_block_locked(ptr);
    if (!block || block->magic != KHEAP_MAGIC_ALLOC || block->is_free) {
        /* Double-free, interior pointer, or corrupt header: count and
         * ignore instead of corrupting the free list. */
        __sync_fetch_and_add(&st_bad, 1);
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }
    /* Poison payload to make use-after-free visible in debug. */
    memset(ptr, KHEAP_POISON_FREE, block->size);
    block->is_free = 1;
    block->magic = KHEAP_MAGIC_FREE;
    st_frees++;
    st_used -= block->size;

    // Coalesce: Merge consecutive free blocks to prevent fragmentation
    kheap_coalesce();
    spin_unlock_irqrestore(&heap_lock, flags);
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr)
        return kmalloc(size);

    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    if (size > SIZE_MAX - 15) {
        __sync_fetch_and_add(&st_fails, 1);
        return NULL;
    }
    size = (size + 15) & ~15;

    unsigned long flags;
    spin_lock_irqsave(&heap_lock, &flags);
    struct heap_block* block = find_block_locked(ptr);
    if (!block || block->magic != KHEAP_MAGIC_ALLOC || block->is_free) {
        __sync_fetch_and_add(&st_bad, 1);
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }

    if (block->size >= size) {
        if (block->size >= size + sizeof(struct heap_block) + 16) {
            struct heap_block* new_block = (struct heap_block*)((uintptr_t)block + sizeof(struct heap_block) + size);
            new_block->magic = KHEAP_MAGIC_FREE;
            new_block->size = block->size - size - sizeof(struct heap_block);
            new_block->is_free = 1;
            new_block->next = block->next;
            st_used -= (block->size - size);
            block->size = size;
            block->next = new_block;
            kheap_coalesce();
        }
        spin_unlock_irqrestore(&heap_lock, flags);
        return ptr;
    }

    size_t old_size = block->size;
    spin_unlock_irqrestore(&heap_lock, flags);

    void* new_ptr = kmalloc(size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);

    return new_ptr;
}

void kheap_stats(uint64_t *used, uint64_t *total, uint64_t *allocs,
                 uint64_t *frees, uint64_t *fails, uint64_t *bad_frees) {
    if (used) *used = st_used;
    if (total) *total = heap_end - HEAP_START;
    if (allocs) *allocs = st_allocs;
    if (frees) *frees = st_frees;
    if (fails) *fails = st_fails;
    if (bad_frees) *bad_frees = st_bad;
}

void itoa(int64_t val, char *buf, int base) {
    if (base < 2 || base > 36) return;

    char *ptr = buf;
    char *ptr1 = ptr;
    char tmp_char;
    int64_t tmp_val;

    if (val == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }

    if (val < 0 && base == 10) {
        *ptr++ = '-';
        val = -val;
    }

    while (val != 0) {
        tmp_val = val;
        val /= base;
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[tmp_val - val * base];
    }

    *ptr = '\0';

    if (*buf == '-') ptr1++;

    while (ptr1 < ptr - 1) {
        tmp_char = *ptr1;
        *ptr1 = *(ptr - 1);
        *--ptr = tmp_char;
        ptr1++;
    }
}
