#include <task.h>
#include <kheap.h>
#include <pmm.h>
#include <vmm.h>
#include <screen.h>
#include <idt.h>
#include <serial.h>
#include <tty.h>
#include <panic.h>
#include <stddef.h>
#include <gdt.h>
#include <dos/dos.h>
#include <msgport.h>
#include <bsdsocket.h>

/* task_switch.asm usa offsets hardcoded da task_struct_t. Se a struct mudar,
 * o build quebra aqui em vez de trocar de stack para lixo em runtime
 * (foi exatamente isso que gerou RSP=0 + Double Fault: TASK_RSP valia 96,
 * que é o offset de rbp, enquanto rsp está em 88). */
_Static_assert(offsetof(task_struct_t, rsp) == 88,
               "TASK_RSP em kernel/task_switch.asm fora de sincronia com task_struct_t.rsp");

#define kheap_alloc(size) kmalloc(size)
#define kheap_free(ptr) kfree(ptr)

task_struct_t *current = NULL;
task_struct_t *task_list = NULL;

/* ================================================================== */
/* task_trampoline — first code a new kernel task runs via ret          */
/* context_switch() pops callee-saved regs then does ret here.         */
/* We fetch the real entry from current->rip, call it, then exit.      */
/* ================================================================== */

void task_trampoline(void) {
    void (*entry)(void) = (void (*)(void))current->rip;
    entry();
    task_exit(0);
}
volatile uint64_t need_resched = 0;
static uint64_t next_pid = 1;

static spinlock_irq_t runqueue_lock = { SPINLOCK_INIT, 0 };

/* ================================================================== */
/* FPU lazy restore — #NM handler (vector 7, Device Not Available)     */
/* Only saves/restores FPU when a task actually uses floating-point.   */
/* ================================================================== */
static task_struct_t *fpu_owner = NULL;

static void fpu_nm_handler(struct interrupt_frame *frame __attribute__((unused))) {
    /* Save previous FPU owner's state */
    if (fpu_owner && fpu_owner != current) {
        fpu_save(fpu_owner->fxsave_area);
    }
    /* Restore current task's FPU state (or initialize if first use) */
    if (current->fpu_used) {
        fpu_restore(current->fxsave_area);
    } else {
        fpu_init();
        current->fpu_used = true;
    }
    fpu_owner = current;
    /* Clear CR0.TS so FPU instructions won't trap again */
    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "and $~0x8, %%rax\n"
        "mov %%rax, %%cr0\n"
        ::: "rax", "memory"
    );
}

#define RUNQUEUE_LOCK(flags) spin_lock_irqsave(&runqueue_lock, &(flags))
#define RUNQUEUE_UNLOCK(flags) spin_unlock_irqrestore(&runqueue_lock, flags)

/* ================================================================== */
/* task_init — init_task (PID 1, kernel idle)                          */
/* ================================================================== */

void task_init(void) {
    serial_print("AgnusOS: task_init entry\n");
    task_struct_t *init_task = (task_struct_t *)kheap_alloc(sizeof(task_struct_t));
    if (!init_task) {
        serial_print("AgnusOS: task_init - kheap_alloc failed\n");
        return;
    }
    serial_print("AgnusOS: task_init - task allocated\n");

    init_task->pid = next_pid++;
    init_task->tid = init_task->pid;
    init_task->state = TASK_STATE_RUNNING;
    init_task->exit_code = 0;
    init_task->parent = NULL;
    init_task->children = NULL;
    init_task->next_sibling = NULL;
    init_task->prev_sibling = NULL;

    extern vfs_node_t *vfs_root;
    init_task->cwd = vfs_root;

    init_task->mm = (mm_struct_t *)kheap_alloc(sizeof(mm_struct_t));
    serial_print("AgnusOS: task_init - mm allocated\n");
    if (init_task->mm) {
        extern uint64_t kernel_pml4_phys;
        init_task->mm->pml4_phys = kernel_pml4_phys;
        init_task->mm->start_stack = 0;
        spinlock_init(&init_task->mm->lock.lock);
        serial_print("AgnusOS: task_init - mm spinlock init done\n");
        init_task->mm->refcount = 1;
        init_task->mm->vmas = NULL;
    }

    init_task->kernel_stack = (uint64_t)kheap_alloc(16384) + 16384;
    serial_print("AgnusOS: task_init - kernel_stack allocated\n");
    init_task->user_stack = 0;

    init_task->rip = 0;
    init_task->rsp = init_task->kernel_stack;
    init_task->rbp = 0;
    init_task->rflags = 0x202;

    init_task->rax = init_task->rbx = init_task->rcx = init_task->rdx = 0;
    init_task->rsi = init_task->rdi = 0;
    init_task->r8 = init_task->r9 = init_task->r10 = init_task->r11 = 0;
    init_task->r12 = init_task->r13 = init_task->r14 = init_task->r15 = 0;

    init_task->fs_base = init_task->gs_base = 0;

    /* Exec signal bitmask */
    init_task->sig_recv = 0;
    init_task->sig_wait = 0;
    init_task->sig_except = 0;
    serial_print("AgnusOS: task_init - signal bitmask init done\n");

    /* FPU */
    serial_print("AgnusOS: task_init - before fpu_init\n");
    serial_print("AgnusOS: task_init - skipped fpu_init\n");
    serial_print("AgnusOS: task_init - calling fpu_save\n");
    serial_print("AgnusOS: task_init - skipped fpu_save\n");
    init_task->fpu_used = false;

    serial_print("AgnusOS: task_init - waitqueue init done\n");

    spinlock_init(&runqueue_lock.lock);
    serial_print("AgnusOS: task_init - runqueue lock init done\n");

    init_task->ticks = 0;
    init_task->priority = 10;
    init_task->counter = 10;
    init_task->errno_val = 0;
    init_task->tty = console_tty;
    init_task->socket_base = NULL;
    init_task->name[0] = 'I'; init_task->name[1] = 'n'; init_task->name[2] = 'i';
    init_task->name[3] = 't'; init_task->name[4] = 0;

    init_task->next = init_task;
    init_task->prev = init_task;
    task_list = init_task;
    serial_print("AgnusOS: task_init - task linked\n");

    current = init_task;
    serial_print("AgnusOS: task_init - current set\n");

    /* Register #NM (Device Not Available) handler for lazy FPU restore */
    interrupts_register_handler(7, fpu_nm_handler);
    serial_print("AgnusOS: task_init - FPU lazy restore (#NM handler) registered\n");

    /* Enable CR0.TS so first FPU use triggers #NM for lazy init */
    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "or $0x8, %%rax\n"
        "mov %%rax, %%cr0\n"
        ::: "rax", "memory"
    );

    screen_log("OK", COLOR_LIGHT_GREEN, "Task scheduler initialized (preemptive, FPU lazy).");
}

/* ================================================================== */
/* task_create — new kernel task                                       */
/* ================================================================== */

task_struct_t *task_create(void (*entry)(void), uint64_t flags __attribute__((unused))) {
    if (!current) return NULL;

    task_struct_t *task = (task_struct_t *)kheap_alloc(sizeof(task_struct_t));
    if (!task) return NULL;

    task->pid = next_pid++;
    task->tid = task->pid;
    task->state = TASK_STATE_RUNNING;
    task->exit_code = 0;
    task->parent = current;
    task->children = NULL;
    task->next_sibling = current->children;
    task->prev_sibling = NULL;
    if (current->children) current->children->prev_sibling = task;
    current->children = task;
    task->cwd = current->cwd;

    task->mm = current->mm;
    if (task->mm) task->mm->refcount++;

    task->kernel_stack = (uint64_t)kheap_alloc(16384) + 16384;
    task->user_stack = 0;

    /* Initialize FPU state for new task */
    fpu_init();
    fpu_save(task->fxsave_area);
    task->fpu_used = true;

    /* Exec signal bitmask — clear on fork */
    task->sig_recv = 0;
    task->sig_wait = 0;
    task->sig_except = 0;

    /* Build initial kernel stack for ret-based context switch.
     *
     * context_switch pops r15-rbx then does `ret`.  The return address
     * must be `task_trampoline`, which reads current->rip (the real
     * entry fn) and calls it.  When entry() returns, trampoline calls
     * task_exit(0).
     *
     * Stack (high → low):
     *   [trampoline]   <- ret jumps here after restoring callee-saved regs
     *   [rbx = 0]
     *   [rbp = 0]
     *   [r12 = 0]
     *   [r13 = 0]
     *   [r14 = 0]
     *   [r15 = 0]     <- rsp
     */
    extern void task_trampoline(void);
    uint64_t *sp = (uint64_t *)task->kernel_stack; /* top of 16K block */
    *--sp = (uint64_t)task_trampoline;  /* return address for ret */
    *--sp = 0; /* rbx */
    *--sp = 0; /* rbp */
    *--sp = 0; /* r12 */
    *--sp = 0; /* r13 */
    *--sp = 0; /* r14 */
    *--sp = 0; /* r15 */
    task->rsp = (uint64_t)sp;
    task->rbp = 0;

    task->rip = (uint64_t)entry;
    task->rflags = 0x202;

    task->rax = task->rbx = task->rcx = task->rdx = 0;
    task->rsi = task->rdi = 0;
    task->r8 = task->r9 = task->r10 = task->r11 = 0;
    task->r12 = task->r13 = task->r14 = task->r15 = 0;

    task->fs_base = task->gs_base = 0;

    task->ticks = 0;
    task->priority = 10;
    task->counter = 10;
    task->errno_val = 0;
    task->tty = console_tty;
    task->socket_base = NULL;
    task->name[0] = 0;

    unsigned long irq_flags;
    RUNQUEUE_LOCK(irq_flags);
    task->next = task_list->next;
    task->prev = task_list;
    task_list->next->prev = task;
    task_list->next = task;
    RUNQUEUE_UNLOCK(irq_flags);

    return task;
}

/* ================================================================== */
/* task_exit — immediate cleanup (no zombie state)                     */
/* ================================================================== */

void task_exit(int code) {
    if (!current) {
        PANIC("task_exit called without a current task");
        __builtin_unreachable();
    }

    /* P0 lifecycle: release per-task resources before descheduling.
     * Runs without runqueue lock (VFS/IPC take their own locks).
     * Sockets already had bsdsocket_task_exit() but no caller;
     * DOS handles and MsgPorts are now owner-tagged (pid) and purged here. */
    if (current->socket_base) {
        bsdsocket_task_exit(current->socket_base);
        current->socket_base = NULL;
    }
    dos_task_cleanup(current);
    msgport_task_cleanup(current);

    unsigned long flags;
    RUNQUEUE_LOCK(flags);

    current->state = TASK_STATE_SUSPENDED;
    current->exit_code = code;
    current->sig_recv |= SIGBIT_END;

    if (current->mm) {
        mm_struct_t *mm = current->mm;
        current->mm = NULL;
        mm->refcount--;
        if (mm->refcount == 0) {
            extern uint64_t kernel_pml4_phys;
            if (mm->pml4_phys != 0 && mm->pml4_phys != kernel_pml4_phys) {
                /* The exiting task may still be running on this address space.
                 * Switch to the permanent kernel page table before releasing
                 * any paging structures referenced by CR3. Kernel stacks and
                 * scheduler code are present in the shared kernel mappings. */
                vmm_activate_pml4(kernel_pml4_phys);
                vmm_free_pml4(mm->pml4_phys);
            }
            vma_clear(mm);
            kfree(mm);
        }
    }

    RUNQUEUE_UNLOCK(flags);
    /* schedule() → context_switch() não toca RFLAGS: sem este sti, um
     * task_exit vindo de trap com IF=0 (syscall_entry dá cli; exceções
     * entram com IF=0) calaria o PIT para sempre — ticks congelados, hang
     * mudo. task_exit nunca retorna, então reabilitar aqui é seguro. */
    __asm__ volatile("sti" ::: "memory");
    schedule();

    /* A suspended task is never eligible to run again. Reaching this point
     * would otherwise return through a syscall or exception frame belonging to
     * a task whose address space may already have been destroyed. */
    PANIC("task_exit returned for task %d", current ? current->pid : 0);
    __builtin_unreachable();
}

/* ================================================================== */
/* schedule — preemptive round-robin                                   */
/* ================================================================== */

void schedule(void) {
    if (!current) return;

    unsigned long flags;
    RUNQUEUE_LOCK(flags);

    /* Boost sleeping tasks */
    task_struct_t *t = task_list;
    do {
        if (t != current && t->state == TASK_STATE_RUNNING) {
            if (t->priority < 20) t->priority++;
            if (t->counter < t->priority) t->counter = t->priority;
        }
        t = t->next;
    } while (t != task_list);

    /* Decay current task */
    if (current->state == TASK_STATE_RUNNING) {
        if (current->priority > 1) current->priority--;
        if (current->counter < current->priority) current->counter = current->priority;
    }

    /* Pick next RUNNING task */
    task_struct_t *next = current->next;
    while (next != current) {
        if (next->state == TASK_STATE_RUNNING) break;
        next = next->next;
    }

    if (next == current) {
        if (current->state == TASK_STATE_RUNNING) {
            RUNQUEUE_UNLOCK(flags);
            return;
        }
        /* Force PID 1 (init/idle) */
        task_struct_t *t2 = task_list;
        do {
            if (t2->pid == 1) { next = t2; break; }
            t2 = t2->next;
        } while (t2 != task_list);
        if (next->state != TASK_STATE_RUNNING)
            next->state = TASK_STATE_RUNNING;
    }

    /* No actual switch needed */
    if (next == current) {
        RUNQUEUE_UNLOCK(flags);
        return;
    }

    /* Lazy FPU: save outgoing task's FPU only if it's the current FPU owner.
     * The #NM handler will save/restore on-demand. */
    if (fpu_owner == current && current->fpu_used) {
        fpu_save(current->fxsave_area);
        fpu_owner = NULL;
    }

    task_struct_t *prev = current;
    current = next;
    current->counter = current->priority;

    /* TSS RSP0 must follow every switch: traps/IRQs from Ring 3 land on
     * this stack (CPU loads it on privilege change). A stale RSP0 would
     * route a user trap into another task's (or freed) stack. Cheap
     * memory write, no MSR involved. */
    tss_set_kernel_stack(current->kernel_stack);

    /* Activate the incoming address space before switching stacks. This is
     * required for newly-created tasks because their first context switch
     * returns directly into task_trampoline() and never executes the resumed
     * schedule() continuation below. The outgoing kernel stack remains usable
     * because kernel mappings are shared by every process PML4. */
    extern uint64_t kernel_pml4_phys;
    uint64_t target_cr3 = kernel_pml4_phys;
    if (current->mm && current->mm->pml4_phys != 0)
        target_cr3 = current->mm->pml4_phys;
    vmm_activate_pml4(target_cr3);

    /* Release the run-queue lock BEFORE switching stacks.
     * The target task may need to acquire this lock when it
     * eventually calls schedule() itself. */
    RUNQUEUE_UNLOCK(flags);

    /* Switch kernel stacks via assembly context switch.
     * After this returns, we are on the new task's stack. */
    context_switch(prev, next);

    /* Re-acquire lock for the resumed task */
    RUNQUEUE_LOCK(flags);

    /* Lazy FPU: set CR0.TS so first FPU use by this task triggers #NM,
     * which will restore the FPU state on demand. */
    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "or $0x8, %%rax\n"
        "mov %%rax, %%cr0\n"
        ::: "rax", "memory"
    );

    RUNQUEUE_UNLOCK(flags);

}

/* ================================================================== */
/* Wait queue helpers                                                   */
/* ================================================================== */

void wake_up(wait_queue_head_t *q) {
    if (!q) return;
    unsigned long flags;
    spin_lock_irqsave((spinlock_irq_t *)&q->lock, &flags);
    int woken = 0;
    wait_queue_entry_t *entry = q->head;
    while (entry) {
        wait_queue_entry_t *next = entry->next;
        if (entry->task) {
            ((task_struct_t *)entry->task)->state = TASK_STATE_RUNNING;
            woken++;
        }
        entry = next;
    }
    if (woken) {
        extern volatile uint64_t need_resched;
        need_resched = 1;
    }
    spin_unlock_irqrestore((spinlock_irq_t *)&q->lock, flags);
}

void wake_up_one(wait_queue_head_t *q) {
    if (!q) return;
    unsigned long flags;
    spin_lock_irqsave((spinlock_irq_t *)&q->lock, &flags);
    if (q->head && q->head->task)
        ((task_struct_t *)q->head->task)->state = TASK_STATE_RUNNING;
    spin_unlock_irqrestore((spinlock_irq_t *)&q->lock, flags);
}

/* ================================================================== */
/* Exec signal API — bitmask model                                     */
/* ================================================================== */

void send_sig(int sig, task_struct_t *t, int priv) {
    (void)priv;
    if (!t || sig < 0 || sig >= 32) return;
    t->sig_recv |= (1U << sig);
    if (t->state == TASK_STATE_WAITING)
        t->state = TASK_STATE_RUNNING;
}

void force_sig(int sig, task_struct_t *t) {
    send_sig(sig, t, 1);
}

void do_signal(struct interrupt_frame *frame) {
    (void)frame;
    /* No-op: Exec bitmask model — no signal frame delivery */
}
