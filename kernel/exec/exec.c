#include <exec/exec.h>
#include <task.h>
#include <kheap.h>
#include <string.h>
#include <serial.h>
#include <screen.h>
#include <timer.h>
#include <pmm.h>
#include <vmm.h>

/* Global ExecBase pointer */
exec_base_t *SysBase = NULL;

/* ------------------------------------------------------------------ */
/* ExecBase initialization                                              */
/* ------------------------------------------------------------------ */

void exec_init(void) {
    SysBase = (exec_base_t *)kmalloc(sizeof(exec_base_t));
    if (!SysBase) {
        serial_print("EXEC: FATAL — cannot allocate ExecBase\n");
        return;
    }
    memset(SysBase, 0, sizeof(exec_base_t));

    SysBase->eb_Magic     = EXEC_MAGIC;
    SysBase->eb_Version   = EXEC_VERSION;
    SysBase->eb_Revision  = EXEC_REVISION;

    /* Empty task list */
    SysBase->eb_TaskHead  = NULL;
    SysBase->eb_TaskTail  = NULL;
    SysBase->eb_TaskCount = 0;

    /* Empty library list */
    SysBase->eb_LibHead   = NULL;
    SysBase->eb_LibCount  = 0;

    /* Scheduler */
    SysBase->eb_CurrentTask  = NULL;
    SysBase->eb_PreemptCount = 0;
    SysBase->eb_TickCount    = 0;
    spinlock_init(&SysBase->eb_SchedLock.lock);

    /* Memory */
    SysBase->eb_MemTotal = pmm_get_total_memory();
    SysBase->eb_MemFree  = pmm_get_free_memory();

    /* CPU info */
    SysBase->eb_CpuSpeed = 0; /* unknown at boot */
    SysBase->eb_CpuCount = 1;

    serial_print("EXEC: ExecBase initialized at ");
    char buf[32];
    itoa((uint64_t)SysBase, buf, 16);
    serial_print(buf);
    serial_print("\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "Exec (SysBase) inicializado.");
}

/* ------------------------------------------------------------------ */
/* Scheduler control                                                     */
/* ------------------------------------------------------------------ */

void exec_disable(void) {
    __asm__ volatile("cli");
}

void exec_enable(void) {
    __asm__ volatile("sti");
}

/* ------------------------------------------------------------------ */
/* Timing                                                               */
/* ------------------------------------------------------------------ */

uint64_t exec_eclock(void) {
    return SysBase->eb_TickCount;
}

/* ------------------------------------------------------------------ */
/* Memory                                                               */
/* ------------------------------------------------------------------ */

void *exec_alloc_mem(uint32_t size, uint32_t flags) {
    if (size == 0) return NULL;

    void *mem = NULL;

    if (flags & MEMF_CHIP) {
        /* CHIP = VRAM or low DMA-accessible RAM */
        uint64_t phys = pmm_alloc_block();
        if (phys) {
            mem = (void *)(phys + 0xFFFF800000000000ULL);
        }
    } else {
        /* FAST/PUBLIC = kernel heap */
        mem = kmalloc(size);
    }

    if (mem && (flags & MEMF_CLEAR)) {
        memset(mem, 0, size);
    }

    return mem;
}

void exec_free_mem(void *ptr, uint32_t size) {
    (void)size;
    if (!ptr) return;
    /* For now, free from kernel heap */
    kfree(ptr);
}

uint32_t exec_avail_mem(uint32_t flags) {
    (void)flags;
    return (uint32_t)(pmm_get_free_memory() / 1024);
}

/* ------------------------------------------------------------------ */
/* Cache                                                                */
/* ------------------------------------------------------------------ */

void exec_cache_clear(void) {
    /* Flush TLB */
    __asm__ volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        ::: "rax", "memory"
    );
}

void exec_cache_clear_ea(void *addr, uint32_t size) {
    (void)addr; (void)size;
    exec_cache_clear();
}

/* ------------------------------------------------------------------ */
/* Priority                                                              */
/* ------------------------------------------------------------------ */

void exec_set_task_pri(void *task_ptr, int32_t new_pri) {
    task_struct_t *task = (task_struct_t *)task_ptr;
    if (!task) return;
    if (new_pri < 0) new_pri = 0;
    if (new_pri > 127) new_pri = 127;
    task->priority = (uint64_t)new_pri;
}
