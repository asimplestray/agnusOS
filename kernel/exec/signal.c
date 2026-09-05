#include <exec/exec.h>
#include <stddef.h>
#include <task.h>
#include <serial.h>

/* ------------------------------------------------------------------ */
/* Signal mechanism (bitmask-based, AmigaOS-style)                      */
/* ------------------------------------------------------------------ */

static spinlock_irq_t sig_wait_lock = { .lock = SPINLOCK_INIT, .flags = 0 };

void exec_signal(exec_task_t *task, uint32_t mask) {
    if (!task) return;

    unsigned long flags;
    spin_lock_irqsave(&sig_wait_lock, &flags);

    /* Set bits in receiver */
    task->et_SigRecv |= mask;

    /* If the task is waiting for any of these bits, wake it */
    if (task->et_State == EXEC_STATE_WAITING && (task->et_SigWait & mask)) {
        task->et_State = EXEC_STATE_READY;
        task->et_SigWait = 0;
    }

    /* Call exception handler if mask matches and handler is set */
    if (task->et_SigExceptFn && (task->et_SigExcept & mask)) {
        task->et_SigExceptFn(task->et_SigExcept & mask, task);
    }

    spin_unlock_irqrestore(&sig_wait_lock, flags);
}

uint32_t exec_wait(uint32_t mask) {
    exec_task_t *task = SysBase ? SysBase->eb_CurrentTask : NULL;
    if (!task) return 0;

    unsigned long flags;
    spin_lock_irqsave(&sig_wait_lock, &flags);

    /* Check if any bits are already set */
    uint32_t ready = task->et_SigRecv & mask;
    if (ready) {
        task->et_SigRecv &= ~ready;
        spin_unlock_irqrestore(&sig_wait_lock, flags);
        return ready;
    }

    /* Block until signal arrives */
    task->et_SigWait = mask;
    task->et_State = EXEC_STATE_WAITING;

    spin_unlock_irqrestore(&sig_wait_lock, flags);

    /* Yield */
    schedule();

    /* Re-check after wake */
    spin_lock_irqsave(&sig_wait_lock, &flags);
    ready = task->et_SigRecv & mask;
    task->et_SigRecv &= ~ready;
    task->et_SigWait = 0;
    spin_unlock_irqrestore(&sig_wait_lock, flags);

    return ready;
}

uint32_t exec_wait_timeout(uint32_t mask, uint32_t timeout_ticks) {
    exec_task_t *task = SysBase ? SysBase->eb_CurrentTask : NULL;
    if (!task) return 0;

    unsigned long flags;
    spin_lock_irqsave(&sig_wait_lock, &flags);

    /* Check if any bits are already set */
    uint32_t ready = task->et_SigRecv & mask;
    if (ready) {
        task->et_SigRecv &= ~ready;
        spin_unlock_irqrestore(&sig_wait_lock, flags);
        return ready;
    }

    /* Set wake time for timeout */
    task->et_WakeTime = SysBase->eb_TickCount + timeout_ticks;
    task->et_SigWait = mask;
    task->et_State = EXEC_STATE_WAITING;

    spin_unlock_irqrestore(&sig_wait_lock, flags);

    /* Yield */
    schedule();

    /* Re-check after wake */
    spin_lock_irqsave(&sig_wait_lock, &flags);
    ready = task->et_SigRecv & mask;
    task->et_SigRecv &= ~ready;
    task->et_SigWait = 0;
    task->et_WakeTime = 0;
    spin_unlock_irqrestore(&sig_wait_lock, flags);

    return ready;
}

uint32_t exec_set_signal(uint32_t mask) {
    exec_task_t *task = SysBase ? SysBase->eb_CurrentTask : NULL;
    if (!task) return 0;

    unsigned long flags;
    spin_lock_irqsave(&sig_wait_lock, &flags);

    uint32_t old = task->et_SigRecv;
    task->et_SigRecv = mask;

    /* If task is waiting for bits that were just set, wake it */
    if (task->et_State == EXEC_STATE_WAITING && (task->et_SigWait & mask)) {
        task->et_State = EXEC_STATE_READY;
    }

    spin_unlock_irqrestore(&sig_wait_lock, flags);
    return old;
}

uint32_t exec_clear_signal(uint32_t mask) {
    exec_task_t *task = SysBase ? SysBase->eb_CurrentTask : NULL;
    if (!task) return 0;

    unsigned long flags;
    spin_lock_irqsave(&sig_wait_lock, &flags);

    uint32_t old = task->et_SigRecv;
    task->et_SigRecv &= ~mask;

    spin_unlock_irqrestore(&sig_wait_lock, flags);
    return old;
}

uint32_t exec_check_signal(exec_task_t *task) {
    if (!task) return 0;

    unsigned long flags;
    spin_lock_irqsave(&sig_wait_lock, &flags);
    uint32_t bits = task->et_SigRecv;
    spin_unlock_irqrestore(&sig_wait_lock, flags);

    return bits;
}

void exec_cause(exec_task_t *task, uint32_t mask) {
    /* exec_cause sets bits AND calls exception handler */
    exec_signal(task, mask);
}
