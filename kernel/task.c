#include <task.h>
#include <kheap.h>
#include <pmm.h>
#include <vmm.h>
#include <screen.h>
#include <idt.h>
#include <serial.h>
#include <tty.h>

#define kheap_alloc(size) kmalloc(size)
#define kheap_free(ptr) kfree(ptr)

task_struct_t *current = NULL;
task_struct_t *task_list = NULL;
volatile uint64_t need_resched = 0;
static uint64_t next_pid = 1;

static spinlock_irq_t runqueue_lock = { SPINLOCK_INIT, 0 };

#define RUNQUEUE_LOCK(flags) spin_lock_irqsave(&runqueue_lock, &(flags))
#define RUNQUEUE_UNLOCK(flags) spin_unlock_irqrestore(&runqueue_lock, flags)

void task_init(void) {
    serial_print("ApolloOS: task_init entry\n");
    task_struct_t *init_task = (task_struct_t *)kheap_alloc(sizeof(task_struct_t));
    if (!init_task) {
        serial_print("ApolloOS: task_init - kheap_alloc failed\n");
        return;
    }
    serial_print("ApolloOS: task_init - task allocated\n");
    
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
    serial_print("ApolloOS: task_init - mm allocated\n");
    if (init_task->mm) {
        extern uint64_t kernel_pml4_phys;
        init_task->mm->pml4_phys = kernel_pml4_phys;
        init_task->mm->start_code = init_task->mm->end_code = 0;
        init_task->mm->start_data = init_task->mm->end_data = 0;
        init_task->mm->start_brk = init_task->mm->brk = 0;
        init_task->mm->start_stack = 0;
        spinlock_init(&init_task->mm->lock);
        serial_print("ApolloOS: task_init - mm spinlock init done\n");
        init_task->mm->refcount = 1;
    }
    
    init_task->files = (files_struct_t *)kheap_alloc(sizeof(files_struct_t));
    serial_print("ApolloOS: task_init - files allocated\n");
    if (init_task->files) {
        init_task->files->count = 0;
        for (int i = 0; i < 256; i++) init_task->files->fd_array[i] = NULL;
        spinlock_init(&init_task->files->lock);
        serial_print("ApolloOS: task_init - files spinlock init done\n");
    }
    
    init_task->kernel_stack = (uint64_t)kheap_alloc(16384) + 16384;
    serial_print("ApolloOS: task_init - kernel_stack allocated\n");
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
    
    /* Initialize signal handling */
    for (int i = 1; i < NSIG; i++) {
        init_task->sigaction[i].sa_handler = SIG_DFL;
        init_task->sigaction[i].sa_flags = 0;
        init_task->sigaction[i].sa_mask = 0;
    }
    for (int i = 0; i < _NSIG_WORDS; i++) {
        init_task->blocked.sig[i] = 0;
        init_task->pending.sig[i] = 0;
    }
init_task->signal_list = NULL;
    serial_print("ApolloOS: task_init - signal init done\n");
    
    /* Initialize FPU state */
    serial_print("ApolloOS: task_init - before fpu_init\n");
    // fpu_init();
    serial_print("ApolloOS: task_init - skipped fpu_init\n");
    serial_print("ApolloOS: task_init - calling fpu_save\n");
    // Skip FPU save for now - causing issues
    // fpu_save(init_task->fxsave_area);
    serial_print("ApolloOS: task_init - skipped fpu_save\n");
    init_task->fpu_used = false;

    init_waitqueue_head(&init_task->wait_chldexit);
    serial_print("ApolloOS: task_init - waitqueue init done\n");
    
    spinlock_init(&runqueue_lock.lock);
    serial_print("ApolloOS: task_init - runqueue lock init done\n");
    
    init_task->ticks = 0;
    init_task->priority = 10;
    init_task->counter = 10;
    
    init_task->pgid = init_task->pid;
    init_task->sid = init_task->pid;
    init_task->errno_val = 0;
    init_task->tty = console_tty;
    
    init_task->next = init_task;
    init_task->prev = init_task;
    task_list = init_task;
    serial_print("ApolloOS: task_init - task linked\n");

    current = init_task;
    serial_print("ApolloOS: task_init - current set\n");
    
    screen_log("OK", COLOR_LIGHT_GREEN, "Task scheduler initialized (preemptive).");
}

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
    
    task->files = current->files;
    if (task->files) task->files->count++;
    
    task->kernel_stack = (uint64_t)kheap_alloc(16384) + 16384;
    task->user_stack = 0;
    
    /* Initialize FPU state for new task */
    fpu_init();
    fpu_save(task->fxsave_area);
    task->fpu_used = true;

    /* Inherit signal handlers */
    for (int i = 1; i < NSIG; i++) {
        task->sigaction[i] = current->sigaction[i];
    }
    for (int i = 0; i < _NSIG_WORDS; i++) {
        task->blocked.sig[i] = current->blocked.sig[i];
        task->pending.sig[i] = 0;
    }
    task->signal_list = NULL;
    
    // Build a synthetic interrupt frame at the top of the new task's kernel
    // stack so that resuming it through the IRQ stub's restore path jumps to
    // `entry` with a valid CS/RFLAGS. QEMU's 64-bit iretq always pops a
    // ring-transition frame (5 qwords: RIP, CS, RFLAGS, RSP, SS), so the
    // frame must include the RSP/SS slots or the first resume would consume
    // the kheap block header sitting above the stack top. Layout (low -> high):
    //   R15,R14,...,RAX, int_no, error, RIP, CS, RFLAGS, RSP, SS
    uint64_t *sp = (uint64_t *)task->kernel_stack;
    *--sp = 0x10;                   // SS (kernel data segment)
    *--sp = (uint64_t)task->kernel_stack - 0x28;   // RSP (resume stack)
    *--sp = 0x202;                  // RFLAGS (IF set)
    *--sp = 0x08;                   // CS (kernel code segment)
    *--sp = (uint64_t)entry;        // RIP
    *--sp = 0;                      // error code
    *--sp = 0;                      // interrupt number
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  // rax,rbx,rcx,rdx,rsi
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  // rdi,rbp,r8,r9,r10
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;  // r11,r12,r13,r14,r15
    task->rsp = (uint64_t)sp;       // points at the R15 slot
    task->rbp = 0;
    
    task->rip = (uint64_t)entry;
    task->rflags = 0x202;

    task->rax = task->rbx = task->rcx = task->rdx = 0;
    task->rsi = task->rdi = 0;
    task->r8 = task->r9 = task->r10 = task->r11 = 0;
    task->r12 = task->r13 = task->r14 = task->r15 = 0;
    
    task->fs_base = task->gs_base = 0;
    
    init_waitqueue_head(&task->wait_chldexit);
    
    task->ticks = 0;
    task->priority = 10;
    task->counter = 10;
    
    task->pgid = task->pid;
    task->sid = current->sid;
    task->errno_val = 0;
    task->tty = console_tty;
    
    unsigned long irq_flags;
    RUNQUEUE_LOCK(irq_flags);
    task->next = task_list->next;
    task->prev = task_list;
    task_list->next->prev = task;
    task_list->next = task;
    RUNQUEUE_UNLOCK(irq_flags);
    
    return task;
}

void task_exit(int code) {
    if (!current) return;
    
    unsigned long flags;
    RUNQUEUE_LOCK(flags);
    
    current->state = TASK_STATE_ZOMBIE;
    current->exit_code = code;
    
    if (current->mm) {
        current->mm->refcount--;
        if (current->mm->refcount == 0) {
            extern uint64_t kernel_pml4_phys;
            if (current->mm->pml4_phys != 0 && current->mm->pml4_phys != kernel_pml4_phys) {
                vmm_free_pml4(current->mm->pml4_phys);
            }
            kfree(current->mm);
        }
    }
    if (current->parent) {
        wake_up(&current->parent->wait_chldexit);
    }
    
    RUNQUEUE_UNLOCK(flags);
    
    schedule();
}

void schedule(void) {
    if (!current) return;
    
    unsigned long flags;
    RUNQUEUE_LOCK(flags);

    /* Save current task's FPU state if it was used */
    if (current->fpu_used) {
        fpu_save(current->fxsave_area);
        /* Set CR0.TS to trigger #NM on next FPU use (lazy FPU) */
        __asm__ volatile(
            "mov %%cr0, %%rax\n"
            "or $0x8, %%rax\n"   // Set TS (bit 3)
            "mov %%rax, %%cr0\n"
            ::: "rax", "memory"
        );
    }
    
    /* Priority decay: boost priority of waiting tasks, decay running task */
    task_struct_t *t = task_list;
    do {
        if (t != current && t->state == TASK_STATE_RUNNING) {
            if (t->priority < 20) t->priority++;
            if (t->counter < t->priority) t->counter = t->priority;
        }
        t = t->next;
    } while (t != task_list);

    if (current->state == TASK_STATE_RUNNING) {
        if (current->priority > 1) current->priority--;
        if (current->counter < current->priority) current->counter = current->priority;
    }

    task_struct_t *next = current->next;
    
    while (next != current) {
        if (next->state == TASK_STATE_RUNNING) {
            break;
        }
        next = next->next;
    }
    
    if (next == current) {
        if (current->state == TASK_STATE_RUNNING) {
            RUNQUEUE_UNLOCK(flags);
            return;
        }
        /* Fallback to init_task (acts as the idle task and is always runnable) */
        task_struct_t *t2 = task_list;
        do {
            if (t2->pid == 1) {
                next = t2;
                break;
            }
            t2 = t2->next;
        } while (t2 != task_list);
        if (next->state != TASK_STATE_RUNNING) {
            next->state = TASK_STATE_RUNNING;
        }
    }
    
    current = next;
    current->counter = current->priority;

    /* Restore next task's FPU state */
    if (current->fpu_used) {
        fpu_restore(current->fxsave_area);
        /* Clear CR0.TS since we're loading FPU state */
        __asm__ volatile(
            "mov %%cr0, %%rax\n"
            "and $~0x8, %%rax\n"  // Clear TS (bit 3)
            "mov %%rax, %%cr0\n"
            ::: "rax", "memory"
        );
    }
    
    RUNQUEUE_UNLOCK(flags);

    extern uint64_t kernel_pml4_phys;
    uint64_t target_cr3 = kernel_pml4_phys;
    if (current->mm && current->mm->pml4_phys != 0) {
        target_cr3 = current->mm->pml4_phys;
    }

    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_cr3 != target_cr3) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");
    }
}

void wake_up(wait_queue_head_t *q) {
    if (!q) return;
    unsigned long flags;
    spin_lock_irqsave((spinlock_irq_t *)&q->lock, &flags);
    wait_queue_entry_t *entry = q->head;
    while (entry) {
        wait_queue_entry_t *next = entry->next;
        if (entry->task) {
            ((task_struct_t *)entry->task)->state = TASK_STATE_RUNNING;
        }
        entry = next;
    }
    spin_unlock_irqrestore((spinlock_irq_t *)&q->lock, flags);
}

void wake_up_one(wait_queue_head_t *q) {
    if (!q) return;
    unsigned long flags;
    spin_lock_irqsave((spinlock_irq_t *)&q->lock, &flags);
    if (q->head && q->head->task) {
        ((task_struct_t *)q->head->task)->state = TASK_STATE_RUNNING;
    }
    spin_unlock_irqrestore((spinlock_irq_t *)&q->lock, flags);
}

/* -------------------------------------------------------------------------
 * Signal handling — Exec bitmask model (replaces POSIX signal frames)
 * ---------------------------------------------------------------------- */

void force_sig(int sig, task_struct_t *t) {
    send_sig(sig, t, 1);
}

void send_sig(int sig, task_struct_t *t, int priv) {
    (void)priv;
    if (!t || sig <= 0 || sig >= NSIG) return;
    t->blocked.sig[(sig - 1) / 64] |= (1ULL << ((sig - 1) % 64));
    if (t->state == TASK_STATE_INTERRUPTIBLE)
        t->state = TASK_STATE_RUNNING;
}

void do_signal(struct interrupt_frame *frame) {
    (void)frame;
    /* No-op: Exec bitmask model — no signal frame delivery */
}