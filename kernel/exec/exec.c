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
/* Task management                                                      */
/* ------------------------------------------------------------------ */

static uint32_t next_pid = 1;

exec_task_t *exec_create_task(const char *name, int32_t priority,
                               void (*entry)(void), uint32_t stacksize) {
    /* Allocate task struct */
    exec_task_t *task = (exec_task_t *)kmalloc(sizeof(exec_task_t));
    if (!task) return NULL;
    memset(task, 0, sizeof(exec_task_t));

    /* Name */
    if (name) {
        int i;
        for (i = 0; i < TASK_NAME_MAX - 1 && name[i]; i++)
            task->et_Name[i] = name[i];
        task->et_Name[i] = 0;
    }

    /* Identity */
    task->et_Pid = next_pid++;
    task->et_Flags = 0;

    /* Signals */
    task->et_SigRecv    = 0;
    task->et_SigWait    = 0;
    task->et_SigExcept  = 0;
    task->et_SigExceptFn = NULL;

    /* State */
    task->et_State    = EXEC_STATE_READY;
    task->et_Priority = priority;
    task->et_BasePri  = 0;

    /* Stack — allocate from kernel heap */
    if (stacksize < 4096) stacksize = 4096;
    task->et_StackBase = kmalloc(stacksize);
    if (!task->et_StackBase) {
        kfree(task);
        return NULL;
    }
    task->et_StackSize = stacksize;

    /* Set up kernel thread entry point on the stack */
    uint64_t stack_top = (uint64_t)task->et_StackBase + stacksize;
    /* Align to 16 bytes */
    stack_top &= ~0xFUL;

    /* Initial register state — the task starts at entry */
    task->et_Rip  = (uint64_t)entry;
    task->et_Rsp  = stack_top;
    task->et_Rbp  = stack_top;
    task->et_Rflags = 0x200; /* IF = 1 */

    /* All other registers zero */
    task->et_Rax = task->et_Rbx = task->et_Rcx = task->et_Rdx = 0;
    task->et_Rsi = task->et_Rdi = 0;
    task->et_R8 = task->et_R9 = task->et_R10 = task->et_R11 = 0;
    task->et_R12 = task->et_R13 = task->et_R14 = task->et_R15 = 0;

    /* FPU */
    task->et_FpuUsed = 0;

    /* Memory — no own address space (kernel thread) */
    task->et_Mm = NULL;

    /* MsgPort — none by default */
    task->et_PortId = -1;

    /* Timing */
    task->et_WakeTime = 0;
    task->et_Ticks    = 0;

    /* Exit */
    task->et_ExitCode = 0;

    /* Links */
    task->et_Next = task->et_Prev = NULL;
    task->et_GlobalNext = task->et_GlobalPrev = NULL;

    /* Add to global task list */
    unsigned long flags;
    spin_lock_irqsave(&SysBase->eb_SchedLock, &flags);

    if (SysBase->eb_TaskTail) {
        SysBase->eb_TaskTail->et_GlobalNext = task;
        task->et_GlobalPrev = SysBase->eb_TaskTail;
    } else {
        SysBase->eb_TaskHead = task;
    }
    SysBase->eb_TaskTail = task;
    SysBase->eb_TaskCount++;

    spin_unlock_irqrestore(&SysBase->eb_SchedLock, flags);

    /* Log */
    serial_print("EXEC: CreateTask \"");
    serial_print(task->et_Name);
    serial_print("\" pid=");
    char buf[16];
    itoa(task->et_Pid, buf, 10);
    serial_print(buf);
    serial_print(" pri=");
    itoa(priority, buf, 10);
    serial_print(buf);
    serial_print("\n");

    return task;
}

void exec_delete_task(exec_task_t *task) {
    if (!task) return;

    unsigned long flags;
    spin_lock_irqsave(&SysBase->eb_SchedLock, &flags);

    /* Remove from global list */
    if (task->et_GlobalPrev)
        task->et_GlobalPrev->et_GlobalNext = task->et_GlobalNext;
    else
        SysBase->eb_TaskHead = task->et_GlobalNext;

    if (task->et_GlobalNext)
        task->et_GlobalNext->et_GlobalPrev = task->et_GlobalPrev;
    else
        SysBase->eb_TaskTail = task->et_GlobalPrev;

    SysBase->eb_TaskCount--;

    spin_unlock_irqrestore(&SysBase->eb_SchedLock, flags);

    /* Free stack */
    if (task->et_StackBase) kfree(task->et_StackBase);

    /* Free task struct */
    kfree(task);
}

/* ------------------------------------------------------------------ */
/* Scheduler control                                                     */
/* ------------------------------------------------------------------ */

void exec_forbid(void) {
    unsigned long flags;
    spin_lock_irqsave(&SysBase->eb_SchedLock, &flags);
    SysBase->eb_PreemptCount++;
    spin_unlock_irqrestore(&SysBase->eb_SchedLock, flags);
}

void exec_permit(void) {
    unsigned long flags;
    spin_lock_irqsave(&SysBase->eb_SchedLock, &flags);
    if (SysBase->eb_PreemptCount > 0)
        SysBase->eb_PreemptCount--;
    spin_unlock_irqrestore(&SysBase->eb_SchedLock, flags);
}

void exec_disable(void) {
    __asm__ volatile("cli");
}

void exec_enable(void) {
    __asm__ volatile("sti");
}

/* ------------------------------------------------------------------ */
/* Timing                                                               */
/* ------------------------------------------------------------------ */

void exec_delay(uint32_t ticks) {
    exec_task_t *task = SysBase->eb_CurrentTask;
    if (!task) return;
    task->et_WakeTime = SysBase->eb_TickCount + ticks;
    task->et_State = EXEC_STATE_WAITING;
    /* Yield — schedule() will pick us up when we wake */
    __asm__ volatile("hlt");
}

uint64_t exec_eclock(void) {
    return SysBase->eb_TickCount;
}

/* Called by the PIT timer IRQ handler to update tick count */
void exec_tick(void) {
    if (SysBase) SysBase->eb_TickCount++;

    /* Wake up sleeping tasks */
    exec_task_t *t = SysBase->eb_TaskHead;
    while (t) {
        if (t->et_State == EXEC_STATE_WAITING && t->et_WakeTime > 0 &&
            SysBase->eb_TickCount >= t->et_WakeTime) {
            t->et_WakeTime = 0;
            t->et_State = EXEC_STATE_READY;
        }
        t = t->et_GlobalNext;
    }
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
