#ifndef EXEC_EXEC_H
#define EXEC_EXEC_H

#include <stdint.h>
#include <spinlock.h>
#include <exec/types.h>
#include <exec/task.h>
#include <exec/signal.h>
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

/* Task management */
exec_task_t *exec_create_task(const char *name, int32_t priority,
                               void (*entry)(void), uint32_t stacksize);
void     exec_delete_task(exec_task_t *task);

/* Scheduler control */
void     exec_forbid(void);       /* disable preemption */
void     exec_permit(void);       /* enable preemption */
void     exec_disable(void);      /* disable interrupts (CLI) */
void     exec_enable(void);       /* enable interrupts (STI) */

/* Timing */
void     exec_delay(uint32_t ticks);    /* sleep N ticks (1 tick = 10ms) */
uint64_t exec_eclock(void);             /* ticks since boot */

/* Memory */
void    *exec_alloc_mem(uint32_t size, uint32_t flags);
void     exec_free_mem(void *ptr, uint32_t size);
uint32_t exec_avail_mem(uint32_t flags);

/* Cache */
void     exec_cache_clear(void);
void     exec_cache_clear_ea(void *addr, uint32_t size);

/* Priority */
void     exec_set_task_pri(void *task, int32_t new_pri);

#endif
