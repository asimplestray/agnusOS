#ifndef WAIT_H
#define WAIT_H

#include <stdint.h>
#include <stdbool.h>
#include <spinlock.h>
#include <stddef.h>

typedef struct wait_queue_entry {
    struct wait_queue_entry *next;
    struct wait_queue_entry *prev;
    volatile int state;
    void *task;
} wait_queue_entry_t;

typedef struct {
    wait_queue_entry_t *head;
    wait_queue_entry_t *tail;
    spinlock_t lock;
} wait_queue_head_t;

#define WAIT_QUEUE_HEAD_INIT(name) { NULL, NULL, SPINLOCK_INIT }

static inline void init_waitqueue_head(wait_queue_head_t *q) {
    q->head = q->tail = NULL;
    spinlock_init(&q->lock);
}

static inline void wait_queue_add(wait_queue_head_t *q, wait_queue_entry_t *entry) {
    unsigned long flags;
    spin_lock_irqsave((spinlock_irq_t *)&q->lock, &flags);
    entry->next = NULL;
    entry->prev = q->tail;
    if (q->tail) {
        q->tail->next = entry;
    } else {
        q->head = entry;
    }
    q->tail = entry;
    spin_unlock_irqrestore((spinlock_irq_t *)&q->lock, flags);
}

static inline void wait_queue_remove(wait_queue_head_t *q, wait_queue_entry_t *entry) {
    unsigned long flags;
    spin_lock_irqsave((spinlock_irq_t *)&q->lock, &flags);
    if (entry->prev) entry->prev->next = entry->next;
    else q->head = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    else q->tail = entry->prev;
    spin_unlock_irqrestore((spinlock_irq_t *)&q->lock, flags);
}

static inline bool wait_queue_empty(wait_queue_head_t *q) {
    return q->head == NULL;
}

/* Functions implemented in kernel/task.c to avoid circular dependencies */
void wake_up(wait_queue_head_t *q);
void wake_up_one(wait_queue_head_t *q);

#define DEFINE_WAIT(name) \
    wait_queue_entry_t name = { NULL, NULL, 0, NULL }

#define prepare_to_wait(q, wait, state_val) \
    do { \
        wait_queue_remove(q, wait); \
        (wait)->state = (state_val); \
        (wait)->task = (void *)current; \
        wait_queue_add(q, wait); \
    } while (0)

#define finish_wait(q, wait) \
    do { \
        wait_queue_remove(q, wait); \
    } while (0)

#define wait_event(q, condition) \
    do { \
        if (condition) break; \
        DEFINE_WAIT(__wait); \
        for (;;) { \
            prepare_to_wait(&(q), &__wait, TASK_STATE_UNINTERRUPTIBLE); \
            if (condition) break; \
            current->state = TASK_STATE_UNINTERRUPTIBLE; \
            schedule(); \
        } \
        finish_wait(&(q), &__wait); \
    } while (0)

#define wait_event_interruptible(q, condition) \
    ({ \
        int __ret = 0; \
        if (!(condition)) { \
            DEFINE_WAIT(__wait); \
            for (;;) { \
                prepare_to_wait(&(q), &__wait, TASK_STATE_INTERRUPTIBLE); \
                if (condition) break; \
                current->state = TASK_STATE_INTERRUPTIBLE; \
                schedule(); \
            } \
            finish_wait(&(q), &__wait); \
        } \
        __ret; \
    })

#endif