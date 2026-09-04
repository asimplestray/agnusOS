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
 * Signal handling
 * (sigismember, sigaddset, sigdelset are inline in task.h)
 * ---------------------------------------------------------------------- */

void send_sig(int sig, task_struct_t *t, int priv) {
    if (!t || sig <= 0 || sig >= NSIG) return;
    
    /* Check if signal is blocked */
    if (!priv && sigismember(&t->blocked, sig)) {
        sigaddset(&t->pending, sig);
        return;
    }
    
    /* SIGKILL and SIGSTOP cannot be blocked */
    if (sig == SIGKILL || sig == SIGSTOP) {
        /* Force delivery */
    }
    
    /* Add to pending */
    sigaddset(&t->pending, sig);
    
    /* Wake up task if it's waiting */
    if (t->state == TASK_STATE_INTERRUPTIBLE) {
        t->state = TASK_STATE_RUNNING;
    }
    
    /* SIGCONT resumes stopped processes */
    if (sig == SIGCONT && t->state == TASK_STATE_STOPPED) {
        t->state = TASK_STATE_RUNNING;
    }
}

void force_sig(int sig, task_struct_t *t) {
    send_sig(sig, t, 1);
}

/* Check and deliver pending signals */
void do_signal(struct interrupt_frame *frame) {
    if (!current) return;
    
    /* Check for pending signals */
    for (int sig = 1; sig < NSIG; sig++) {
        if (sigismember(&current->pending, sig)) {
            /* Remove from pending */
            sigdelset(&current->pending, sig);
            
            sigaction_t *act = &current->sigaction[sig];
            
            if (act->sa_handler == SIG_IGN) {
                continue; /* Ignored */
            }
            
            if (act->sa_handler == SIG_DFL) {
                /* Default action */
                switch (sig) {
                    case SIGKILL:
                    case SIGTERM:
                    case SIGINT:
                    case SIGQUIT:
                    case SIGSEGV:
                    case SIGBUS:
                    case SIGFPE:
                    case SIGILL:
                    case SIGABRT:
                        task_exit(sig);
                        return;
                    case SIGSTOP:
                    case SIGTSTP:
                    case SIGTTIN:
                    case SIGTTOU:
                        current->state = TASK_STATE_STOPPED;
                        schedule();
                        return;
                    case SIGCONT:
                        /* Resume if stopped */
                        if (current->state == TASK_STATE_STOPPED) {
                            current->state = TASK_STATE_RUNNING;
                        }
                        return;
                    case SIGCHLD:
                    case SIGURG:
                    case SIGWINCH:
                        return; /* Ignore by default */
                    default:
                        task_exit(sig);
                        return;
                }
            }
            
            /* User handler - set up signal frame */
            if (act->sa_handler && act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
                /* Build signal frame on user stack */
                uint64_t sp = current->rsp;
                
                /* Align stack */
                sp &= ~0xF;
                
                /* Push sigreturn context */
                ucontext_t *uc = (ucontext_t *)(sp - sizeof(ucontext_t));
                
                /* Save current context */
                uc->uc_flags = 0;
                uc->uc_link = NULL;
                for (int i = 0; i < _NSIG_WORDS; i++) {
                    uc->uc_sigmask.sig[i] = current->blocked.sig[i];
                }
                
                /* Save register context */
                uc->uc_mcontext.rax = current->rax;
                uc->uc_mcontext.rbx = current->rbx;
                uc->uc_mcontext.rcx = current->rcx;
                uc->uc_mcontext.rdx = current->rdx;
                uc->uc_mcontext.rsi = current->rsi;
                uc->uc_mcontext.rdi = current->rdi;
                uc->uc_mcontext.rbp = current->rbp;
                uc->uc_mcontext.r8  = current->r8;
                uc->uc_mcontext.r9  = current->r9;
                uc->uc_mcontext.r10 = current->r10;
                uc->uc_mcontext.r11 = current->r11;
                uc->uc_mcontext.r12 = current->r12;
                uc->uc_mcontext.r13 = current->r13;
                uc->uc_mcontext.r14 = current->r14;
                uc->uc_mcontext.r15 = current->r15;
                uc->uc_mcontext.rip = frame->rip;
                uc->uc_mcontext.cs = frame->cs;
                uc->uc_mcontext.rflags = frame->rflags;
                uc->uc_mcontext.rsp = frame->rsp;
                uc->uc_mcontext.ss = frame->ss;
                
                /* Update user stack pointer */
                current->rsp = (uint64_t)uc;
                
                /* Set up return to signal handler */
                frame->rip = (uint64_t)act->sa_handler;
                frame->rdi = sig; /* First arg */
                frame->rsp = current->rsp;
                
                /* Block signals during handler if SA_NODEFER not set */
                if (!(act->sa_flags & SA_NODEFER)) {
                    current->blocked.sig[0] |= (1ULL << (sig - 1));
                }
                if (act->sa_mask) {
                    current->blocked.sig[0] |= act->sa_mask;
                }
                
                return;
            }
        }
    }
}

/* Signal syscall implementations */
int64_t aos_signal(int sig, const sigaction_t *act, sigaction_t *oldact, size_t sigsetsize, struct interrupt_frame *frame) {
    (void)frame;
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) {
        return -1; /* EINVAL */
    }
    
    if (oldact) {
        *oldact = current->sigaction[sig];
    }
    
    if (act) {
        current->sigaction[sig] = *act;
    }
    
    return 0;
}

int64_t aos_setsignal(int how, const sigset_t *set, sigset_t *oldset, size_t sigsetsize, struct interrupt_frame *frame) {
    (void)frame;
    if (oldset) {
        for (int i = 0; i < _NSIG_WORDS; i++) {
            oldset[i] = current->blocked.sig[i];
        }
    }
    
    if (set) {
        switch (how) {
            case SIG_BLOCK:
                for (int i = 0; i < _NSIG_WORDS; i++) {
                    current->blocked.sig[i] |= set[i];
                }
                break;
            case SIG_UNBLOCK:
                for (int i = 0; i < _NSIG_WORDS; i++) {
                    current->blocked.sig[i] &= ~set[i];
                }
                break;
            case SIG_SETMASK:
                for (int i = 0; i < _NSIG_WORDS; i++) {
                    current->blocked.sig[i] = set[i];
                }
                break;
            default:
                return -1; /* EINVAL */
        }
    }
    
    /* SIGKILL and SIGSTOP cannot be blocked */
    current->blocked.sig[0] &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    
    return 0;
}

int64_t aos_return_signal(struct interrupt_frame *frame) {
    if (!current || !frame) return -1;
    
    /* The user stack pointer (frame->rsp) points to the ucontext_t structure
     * that was set up by do_signal when delivering the signal. */
    ucontext_t *uc = (ucontext_t *)frame->rsp;
    
    /* Restore signal mask */
    for (int i = 0; i < _NSIG_WORDS; i++) {
        current->blocked.sig[i] = uc->uc_sigmask.sig[i];
    }
    /* SIGKILL and SIGSTOP cannot be blocked */
    current->blocked.sig[0] &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    
    /* Restore register context - we need to write to the interrupt frame
     * on the kernel stack so that iretq will use these values */
    frame->rax = uc->uc_mcontext.rax;
    frame->rbx = uc->uc_mcontext.rbx;
    frame->rcx = uc->uc_mcontext.rcx;
    frame->rdx = uc->uc_mcontext.rdx;
    frame->rsi = uc->uc_mcontext.rsi;
    frame->rdi = uc->uc_mcontext.rdi;
    frame->rbp = uc->uc_mcontext.rbp;
    frame->r8  = uc->uc_mcontext.r8;
    frame->r9  = uc->uc_mcontext.r9;
    frame->r10 = uc->uc_mcontext.r10;
    frame->r11 = uc->uc_mcontext.r11;
    frame->r12 = uc->uc_mcontext.r12;
    frame->r13 = uc->uc_mcontext.r13;
    frame->r14 = uc->uc_mcontext.r14;
    frame->r15 = uc->uc_mcontext.r15;
    frame->rip = uc->uc_mcontext.rip;
    frame->cs = uc->uc_mcontext.cs;
    frame->rflags = uc->uc_mcontext.rflags;
    frame->rsp = uc->uc_mcontext.rsp;
    frame->ss = uc->uc_mcontext.ss;
    
    /* Also update current task struct for consistency */
    current->rax = uc->uc_mcontext.rax;
    current->rbx = uc->uc_mcontext.rbx;
    current->rcx = uc->uc_mcontext.rcx;
    current->rdx = uc->uc_mcontext.rdx;
    current->rsi = uc->uc_mcontext.rsi;
    current->rdi = uc->uc_mcontext.rdi;
    current->rbp = uc->uc_mcontext.rbp;
    current->r8  = uc->uc_mcontext.r8;
    current->r9  = uc->uc_mcontext.r9;
    current->r10 = uc->uc_mcontext.r10;
    current->r11 = uc->uc_mcontext.r11;
    current->r12 = uc->uc_mcontext.r12;
    current->r13 = uc->uc_mcontext.r13;
    current->r14 = uc->uc_mcontext.r14;
    current->r15 = uc->uc_mcontext.r15;
    
    return 0;
}

int64_t aos_send_signal(int64_t pid, int64_t sig, struct interrupt_frame *frame) {
    (void)frame;
    if (sig <= 0 || sig >= NSIG) return -1;
    
    task_struct_t *t = task_list;
    do {
        if (t->pid == (uint64_t)pid) {
            send_sig((int)sig, t, 0);
            return 0;
        }
        t = t->next;
    } while (t != task_list);
    
    return -1; /* ESRCH */
}