#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    volatile uint64_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

static inline void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked) {
            __asm__ volatile("pause");
        }
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->locked);
}

static inline bool spin_trylock(spinlock_t *lock) {
    return !__sync_lock_test_and_set(&lock->locked, 1);
}

typedef struct {
    spinlock_t lock;
    unsigned long flags;
} spinlock_irq_t;

static inline void spin_lock_irqsave(spinlock_irq_t *lock, unsigned long *flags) {
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(*flags) : : "memory");
    spin_lock(&lock->lock);
}

static inline void spin_unlock_irqrestore(spinlock_irq_t *lock, unsigned long flags) {
    spin_unlock(&lock->lock);
    if (flags & (1UL << 9)) {
        __asm__ volatile("sti");
    }
}

typedef struct {
    volatile uint64_t count;
    spinlock_t wait_lock;
} mutex_t;

#define MUTEX_INIT { 0, SPINLOCK_INIT }

static inline void mutex_init(mutex_t *mutex) {
    mutex->count = 1;
    spinlock_init(&mutex->wait_lock);
}

static inline void mutex_lock(mutex_t *mutex) {
    while (1) {
        uint64_t old = __sync_fetch_and_sub(&mutex->count, 1);
        if (old == 1) return;
        __sync_fetch_and_add(&mutex->count, 1);
        while (mutex->count <= 0) {
            __asm__ volatile("pause");
        }
    }
}

static inline void mutex_unlock(mutex_t *mutex) {
    __sync_fetch_and_add(&mutex->count, 1);
}

static inline bool mutex_trylock(mutex_t *mutex) {
    uint64_t old = __sync_fetch_and_sub(&mutex->count, 1);
    if (old == 1) return true;
    __sync_fetch_and_add(&mutex->count, 1);
    return false;
}

#endif