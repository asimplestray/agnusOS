#ifndef EXEC_EXEC_H
#define EXEC_EXEC_H

#include <stdint.h>
#include <spinlock.h>
#include <exec/types.h>
#include <exec/task.h>
#include <exec/library.h>

/* Forward decl for task_struct_t used in exec API */
/* task_struct_t is defined in <task.h> — included by callers */

/*
 * ExecBase — the heart of the Exec kernel.
 *
 * A single global struct that holds pointers to all system lists,
 * the current task, memory stats, and scheduler state.  Equivalent
 * to the ExecBase/SysBase of classic AmigaOS.
 */
typedef struct exec_base {
    uint32_t    eb_Magic;        /* EXEC_MAGIC */
    uint32_t    eb_Version;
    uint32_t    eb_Revision;

    /* Task list (all tasks, doubly-linked) */
    exec_task_t *eb_TaskHead;
    exec_task_t *eb_TaskTail;
    uint32_t     eb_TaskCount;

    /* Library list (singly-linked) */
    library_t   *eb_LibHead;
    uint32_t     eb_LibCount;

    /* Scheduler */
    exec_task_t *eb_CurrentTask;
    spinlock_irq_t eb_SchedLock;
    uint32_t     eb_PreemptCount;  /* 0 = preempt ok, >0 = Forbid()'d */
    uint64_t     eb_TickCount;     /* PIT ticks since boot */

    /* Memory */
    uint64_t     eb_MemTotal;
    uint64_t     eb_MemFree;

    /* Cpu */
    uint32_t     eb_CpuSpeed;      /* MHz */
    uint8_t      eb_CpuCount;
} exec_base_t;

extern exec_base_t *SysBase;

/* Exec API */
void     exec_init(void);

/* Scheduler control */
void     exec_disable(void);      /* disable interrupts (CLI) */
void     exec_enable(void);       /* enable interrupts (STI) */

/* Timing */
uint64_t exec_eclock(void);             /* ticks since boot */

/* Memory */
void    *exec_alloc_mem(uint32_t size, uint32_t flags);
void     exec_free_mem(void *ptr, uint32_t size);
uint32_t exec_avail_mem(uint32_t flags);

/* Memory Pools (AmigaOS Exec style) */
typedef struct mem_pool mem_pool_t;

/* Memory flags (from <exec/types.h>) */
/* #define MEMF_CHIP     (1UL << 0)   DMA-accessible (VRAM, device memory) */
/* #define MEMF_FAST     (1UL << 1)   CPU-only, fastest RAM */
/* #define MEMF_PUBLIC   (1UL << 2)   Shareable between tasks */
#define MEMF_CLEAR    (1UL << 16)  /* Zero on allocation */
#define MEMF_REVERSE  (1UL << 17)  /* Allocate from high addresses down */

mem_pool_t *exec_create_pool(uint32_t flags, uint32_t pudge_size, uint32_t thresh_size);
void        exec_delete_pool(mem_pool_t *pool);
void       *exec_alloc_pooled(mem_pool_t *pool, uint32_t size);
void        exec_free_pooled(mem_pool_t *pool, void *ptr, uint32_t size);
uint32_t    exec_pool_available(mem_pool_t *pool, uint32_t flags);

/* Cache */
void     exec_cache_clear(void);
void     exec_cache_clear_ea(void *addr, uint32_t size);

/* Priority */
void     exec_set_task_pri(void *task, int32_t new_pri);

#endif
