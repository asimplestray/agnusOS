#ifndef EXEC_TASK_H
#define EXEC_TASK_H

#include <stdint.h>
#include <spinlock.h>

#define TASK_NAME_MAX 32

/* Signal bits — 32-bit bitmask, como AmigaOS */
#define SIGF_0          (1UL << 0)
#define SIGF_ABORT      (1UL << 1)
#define SIGF_USER_2     (1UL << 2)
#define SIGF_USER_3     (1UL << 3)
#define SIGF_USER_4     (1UL << 4)
#define SIGF_USER_5     (1UL << 5)
#define SIGF_USER_6     (1UL << 6)
#define SIGF_USER_7     (1UL << 7)
#define SIGF_FPERR      (1UL << 8)
#define SIGF_RETHINK    (1UL << 9)
#define SIGF_SINGLE     (1UL << 10)
#define SIGF_LION       (1UL << 11)
#define SIGF_DOS        (1UL << 29)
#define SIGF_EMPTY      (1UL << 30)
#define SIGF_END_CODE   (1UL << 31)

#define SIGBREAKF_CTRL_C (1UL << 15)
#define SIGBREAKF_CTRL_D (1UL << 16)
#define SIGBREAKF_CTRL_Z (1UL << 17)

/* Task states */
#define EXEC_STATE_READY      0
#define EXEC_STATE_RUNNING    1
#define EXEC_STATE_WAITING    2
#define EXEC_STATE_SUSPENDED  3

/* Priority range */
#define TASK_PRI_MIN  (-128)
#define TASK_PRI_MAX  (+127)
#define TASK_PRI_DEFAULT 0

/* Task flags */
#define TF_PROCES      (1UL << 0)   /* task é um processo (tem PML4 próprio) */
#define TF_NOYSIG      (1UL << 1)   /* não sinaliza SIGF_END_CODE ao morrer */
#define TF_ISLAUNCHED  (1UL << 2)   /* já foi colocada na run queue */

typedef struct exec_task {
    /* Identification */
    char        et_Name[TASK_NAME_MAX];
    uint32_t    et_Pid;
    uint32_t    et_Flags;

    /* Signals (bitmask 32-bit) */
    uint32_t    et_SigRecv;       /* bits recebidos */
    uint32_t    et_SigWait;       /* bits que Wait() espera */
    uint32_t    et_SigExcept;     /* exception mask */
    void      (*et_SigExceptFn)(uint32_t, struct exec_task *);

    /* State */
    uint32_t    et_State;
    int32_t     et_Priority;      /* -128..+127, higher = more urgent */
    int32_t     et_BasePri;       /* prioridade base (Forbid/Permit) */

    /* Stack */
    void       *et_StackBase;
    uint32_t    et_StackSize;

    /* Registers (saved on context switch) */
    uint64_t    et_Rax, et_Rbx, et_Rcx, et_Rdx;
    uint64_t    et_Rsi, et_Rdi;
    uint64_t    et_R8, et_R9, et_R10, et_R11;
    uint64_t    et_R12, et_R13, et_R14, et_R15;
    uint64_t    et_Rbp;
    uint64_t    et_Rip;
    uint64_t    et_Rsp;
    uint64_t    et_Rflags;

    /* FPU */
    uint8_t     et_FpuState[512] __attribute__((aligned(16)));
    uint8_t     et_FpuUsed;

    /* Memory (NULL = shared with parent, or own PML4) */
    struct mm_struct *et_Mm;

    /* MsgPort (porta padrão da task, pode ser NULL) */
    int32_t     et_PortId;

    /* Timing */
    uint64_t    et_WakeTime;      /* para Delay() */
    uint64_t    et_Ticks;         /* CPU ticks acumulados */

    /* Exit */
    int32_t     et_ExitCode;

    /* Run queue links */
    struct exec_task *et_Next;
    struct exec_task *et_Prev;

    /* Task list (global) */
    struct exec_task *et_GlobalNext;
    struct exec_task *et_GlobalPrev;
} exec_task_t;

#endif
