#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include <spinlock.h>
#include <wait.h>
#include <vfs.h>
#include <vmm.h>
#include <idt.h>
#include <serial.h>

#define NR_TASKS 64

/* Signal definitions */
#define NSIG 32
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGPWR   30
#define SIGSYS   31

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002
#define SA_SIGINFO    0x00000004
#define SA_RESTART    0x00000008
#define SA_ONSTACK    0x00000100
#define SA_NODEFER    0x00000400
#define SA_RESETHAND  0x00000800

typedef struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, void *, void *);
    uint64_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
} sigaction_t;

typedef uint64_t sigset_t;

#define _NSIG_WORDS ((NSIG + 63) / 64)

typedef struct {
    uint64_t sig[_NSIG_WORDS];
} kernel_sigset_t;

#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

/* ucontext for sigreturn */
typedef struct {
    uint64_t uc_flags;
    struct ucontext *uc_link;
    kernel_sigset_t uc_sigmask;
    struct {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
        uint64_t rip, cs, rflags, rsp, ss;
    } uc_mcontext;
} ucontext_t;

typedef enum {
    TASK_STATE_RUNNING = 0,
    TASK_STATE_INTERRUPTIBLE = 1,
    TASK_STATE_UNINTERRUPTIBLE = 2,
    TASK_STATE_STOPPED = 4,
    TASK_STATE_ZOMBIE = 8,
} task_state_t;

typedef struct mm_struct {
    uint64_t pml4_phys;
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_brk, brk;
    uint64_t start_stack;
    spinlock_t lock;
    int refcount;
} mm_struct_t;

typedef struct file {
    vfs_node_t *node;
    uint64_t offset;
    int flags;
    int refcount;
} file_t;

typedef struct files_struct {
    file_t *fd_array[256];
    int count;
    spinlock_t lock;
} files_struct_t;

typedef struct task_struct {
    uint64_t pid;
    uint64_t tid;
    task_state_t state;
    int exit_code;
    
    struct task_struct *parent;
    struct task_struct *children;
    struct task_struct *next_sibling;
    struct task_struct *prev_sibling;
    
    mm_struct_t *mm;
    files_struct_t *files;
    
    uint64_t kernel_stack;
    uint64_t user_stack;
    
    uint64_t rip;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rflags;
    
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    
    uint64_t fs_base, gs_base;

    vfs_node_t *cwd;  /* Current working directory in VFS */
    
    wait_queue_head_t wait_chldexit;
    
    uint64_t ticks;
    uint64_t priority;
    uint64_t counter;
    
    /* FPU/SSE state (512 bytes, 16-byte aligned) */
    uint8_t fxsave_area[512] __attribute__((aligned(16)));
    bool fpu_used;
    
    /* Signal handling */
    sigaction_t sigaction[NSIG];
    kernel_sigset_t blocked;
    kernel_sigset_t pending;
    struct sigpending *signal_list;
    
    /* Process group / session for job control */
    uint64_t pgid;
    uint64_t sid;
    
    /* Errno for syscalls */
    int errno_val;
    
    /* TTY for this process */
    struct tty_struct *tty;
    
    struct task_struct *next;
    struct task_struct *prev;
} task_struct_t;

extern task_struct_t *current;
extern task_struct_t *task_list;

task_struct_t *task_create(void (*entry)(void), uint64_t flags);
void task_exit(int code);
void schedule(void);
void task_init(void);
void syscall_init(void);

/* Signal handling */
void do_signal(struct interrupt_frame *frame);
void force_sig(int sig, task_struct_t *t);
void send_sig(int sig, task_struct_t *t, int priv);

/* FPU/SSE management */
static inline void fpu_save(void *buf) {
    serial_print("ApolloOS: fpu_save entry\n");
    // Enable FPU/SSE in CR0: MP=1 (Math Present), EM=0 (No emulation), TS=0 (Task not switched)
    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "and $~0x8, %%rax\n"  // Clear TS (bit 3)
        "or $0x2, %%rax\n"    // Set MP (bit 1)
        "mov %%rax, %%cr0\n"
        ::: "rax", "memory"
    );
    serial_print("ApolloOS: fpu_save CR0 modified\n");
    __asm__ volatile("fxsave (%0)" : : "r"(buf) : "memory");
    serial_print("ApolloOS: fpu_save done\n");
}

static inline void fpu_restore(void *buf) {
    __asm__ volatile("fxrstor (%0)" : : "r"(buf) : "memory");
}

static inline void fpu_init(void) {
    // Enable FPU/SSE in CR0: MP=1 (Math Present), EM=0 (No emulation), TS=0 (Task not switched)
    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "and $~0x8, %%rax\n"  // Clear TS (bit 3)
        "or $0x2, %%rax\n"    // Set MP (bit 1)
        "mov %%rax, %%cr0\n"
        ::: "rax", "memory"
    );
    __asm__ volatile("fninit" ::: "memory");
}

static inline task_struct_t *get_current(void) {
    return current;
}

static inline uint64_t get_pid(void) {
    return current ? current->pid : 0;
}

static inline int sigismember(kernel_sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) return 0;
    return (set->sig[(sig - 1) / 64] >> ((sig - 1) % 64)) & 1;
}

static inline void sigaddset(kernel_sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) return;
    set->sig[(sig - 1) / 64] |= (1ULL << ((sig - 1) % 64));
}

static inline void sigdelset(kernel_sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= NSIG) return;
    set->sig[(sig - 1) / 64] &= ~(1ULL << ((sig - 1) % 64));
}

#endif