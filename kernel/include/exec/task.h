#ifndef EXEC_TASK_H
#define EXEC_TASK_H

#include <stdint.h>
#include <task.h>

/*
 * exec_task_t is the *single* task type used by the kernel.
 * It is an alias of task_struct_t (defined in <task.h>) so the Exec layer
 * and the scheduler share one structure — no parallel task model.
 */
typedef task_struct_t exec_task_t;

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

#endif
