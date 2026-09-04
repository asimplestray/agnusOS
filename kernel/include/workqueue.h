#ifndef WORKQUEUE_H
#define WORKQUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <spinlock.h>

struct work_struct {
    void (*func)(struct work_struct *work);
    struct work_struct *next;
    uint64_t execute_at; /* for delayed work */
    uint8_t delayed;
};

struct workqueue_struct {
    struct work_struct *head;
    struct work_struct *tail;
    spinlock_irq_t lock;
    char name[32];
};

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_long_wq;

void workqueue_init(void);
struct workqueue_struct *alloc_workqueue(const char *name, uint32_t flags);
void destroy_workqueue(struct workqueue_struct *wq);

#define WORK_CPU_UNBOUND 0x0001

#define INIT_WORK(_work, _func) \
    do { \
        (_work)->func = (_func); \
        (_work)->next = NULL; \
        (_work)->delayed = 0; \
        (_work)->execute_at = 0; \
    } while (0)

#define INIT_DELAYED_WORK(_work, _func) \
    do { \
        (_work)->func = (_func); \
        (_work)->next = NULL; \
        (_work)->delayed = 1; \
        (_work)->execute_at = 0; \
    } while (0)

int queue_work(struct workqueue_struct *wq, struct work_struct *work);
int queue_delayed_work(struct workqueue_struct *wq, struct work_struct *work, uint64_t delay_ticks);
int cancel_work_sync(struct work_struct *work);
int cancel_delayed_work_sync(struct work_struct *work);

void flush_workqueue(struct workqueue_struct *wq);
void workqueue_start_kworker(void);

/* Timerwheel for delayed work */
void timerwheel_init(void);
void timerwheel_add_delayed_work(struct work_struct *work, uint64_t delay_ticks);
void timerwheel_process(uint64_t current_tick);

#endif