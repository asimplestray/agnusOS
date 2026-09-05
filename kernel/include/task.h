#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <spinlock.h>
#include <wait.h>
#include <vfs.h>
#include <vmm.h>

/* Forward declarations for bsdsocket.library */
struct SocketBase;
#include <idt.h>
#include <serial.h>

#define NR_TASKS 64

/* ================================================================== */
/* Task states — Exec model (no ZOMBIE, no STOPPED)                    */
/* ================================================================== */

typedef enum {
    TASK_STATE_READY        = 0,   /* Ready to run */
    TASK_STATE_RUNNING      = 1,   /* Currently executing */
    TASK_STATE_WAITING      = 2,   /* Blocked on signal/msgport/timer */
    TASK_STATE_SUSPENDED    = 3,   /* Suspended by another task */
    /* Legacy compat — keep INTERRUPTIBLE/UNINTERRUPTIBLE mapped */
    TASK_STATE_INTERRUPTIBLE = 2,  /* alias for WAITING */
    TASK_STATE_UNINTERRUPTIBLE = 2, /* alias for WAITING */
} task_state_t;

/* ================================================================== */
/* Exec signal bitmask — 32 bits, no POSIX handlers                    */
/* ================================================================== */

#define SIGBIT(n)    (1U << (n))

/* Kernel-internal signal bits */
#define SIGBIT_ABORT    SIGBIT(0)   /* Abort task */
#define SIGBIT_FORCE    SIGBIT(1)   /* Force delivery (vmm fault etc) */
#define SIGBIT_FPERR    SIGBIT(8)   /* FPU error */
#define SIGBIT_BREAK    SIGBIT(15)  /* Ctrl+C */
#define SIGBIT_SUSPEND  SIGBIT(16)  /* Ctrl+Z (stop) */
#define SIGBIT_DOS      SIGBIT(29)  /* DOS operation complete */
#define SIGBIT_SINGLE   SIGBIT(30)  /* Single step */
#define SIGBIT_END      SIGBIT(31)  /* Task terminated */

/* User-available signal bits (bits 2-7, 9-14, 16-28) */
#define SIGBIT_USER_0   SIGBIT(2)
#define SIGBIT_USER_1   SIGBIT(3)
#define SIGBIT_USER_2   SIGBIT(4)
#define SIGBIT_USER_3   SIGBIT(5)
#define SIGBIT_USER_4   SIGBIT(6)
#define SIGBIT_USER_5   SIGBIT(7)
#define SIGBIT_USER_6   SIGBIT(9)
#define SIGBIT_USER_7   SIGBIT(10)

/* Legacy POSIX compat aliases (map to signal bits for send_sig/force_sig) */
#define NSIG    32
#define SIGKILL   0
#define SIGSEGV  11
#define SIGINT    2
#define SIGTSTP  20
#define SIGSTOP  19
#define SIGCONT  18
#define SIGPIPE  13
#define SIGCHLD  17
#define SIGTERM  15
#define SIGBUS    7
#define SIGFPE    8
#define SIGILL    4
#define SIGABRT   6
#define SIGQUIT   3
#define SIGUSR1  10
#define SIGUSR2  12
#define SIGALRM  14

/* ================================================================== */
/* Memory structures                                                    */
/* ================================================================== */

typedef struct mm_struct {
    uint64_t pml4_phys;
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_brk, brk;
    uint64_t start_stack;
    spinlock_t lock;
    int refcount;
} mm_struct_t;

/* ================================================================== */
/* File structures                                                      */
/* ================================================================== */

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

/* ================================================================== */
/* Task structure — Exec bitmask model                                  */
/* ================================================================== */

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

    vfs_node_t *cwd;

    uint64_t ticks;
    uint64_t priority;
    uint64_t counter;

    /* FPU/SSE state (512 bytes, 16-byte aligned) */
    uint8_t fxsave_area[512] __attribute__((aligned(16)));
    bool fpu_used;

    /* Exec signal bitmask (replaces POSIX sigaction/pending/blocked) */
    uint32_t sig_recv;       /* bits received (pending) */
    uint32_t sig_wait;       /* bits we're waiting for (Wait()) */
    uint32_t sig_except;     /* exception handler mask */

    /* Errno for syscalls */
    int errno_val;

    /* TTY for this process */
    struct tty_struct *tty;

    /* bsdsocket.library SocketBase (per-task, AmigaOS convention) */
    struct SocketBase *socket_base;

    /* Process group for TTY job control */
    uint64_t pgid;

    /* Task name (AmigaOS-style) */
    char name[32];

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
void context_switch(task_struct_t *prev, task_struct_t *next);

/* Exec signal API — bitmask model */
void do_signal(struct interrupt_frame *frame);
void force_sig(int sig, task_struct_t *t);
void send_sig(int sig, task_struct_t *t, int priv);

/* FPU/SSE management */
static inline void fpu_save(void *buf) {
    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "and $~0x8, %%rax\n"
        "or $0x2, %%rax\n"
        "mov %%rax, %%cr0\n"
        ::: "rax", "memory"
    );
    __asm__ volatile("fxsave (%0)" : : "r"(buf) : "memory");
}

static inline void fpu_restore(void *buf) {
    __asm__ volatile("fxrstor (%0)" : : "r"(buf) : "memory");
}

static inline void fpu_init(void) {
    __asm__ volatile(
        "mov %%cr0, %%rax\n"
        "and $~0x8, %%rax\n"
        "or $0x2, %%rax\n"
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

#endif
