#include <workqueue.h>
#include <kheap.h>
#include <screen.h>
#include <timer.h>
#include <spinlock.h>
#include <task.h>

#define TIMERWHEEL_SIZE 512  /* Must be power of 2 */

/* Simple string copy */
static void simple_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++)) {}
}

struct timerwheel_bucket {
    struct work_struct *head;
    struct work_struct *tail;
    spinlock_irq_t lock;
};

static struct timerwheel_bucket timerwheel[TIMERWHEEL_SIZE];
static uint64_t timerwheel_current = 0;

struct workqueue_struct *system_wq = NULL;
struct workqueue_struct *system_long_wq = NULL;

void workqueue_init(void) {
    system_wq = (struct workqueue_struct *)kmalloc(sizeof(struct workqueue_struct));
    system_long_wq = (struct workqueue_struct *)kmalloc(sizeof(struct workqueue_struct));
    
    if (!system_wq || !system_long_wq) {
        screen_log("FAIL", COLOR_LIGHT_RED, "Workqueue: OOM");
        return;
    }
    
    system_wq->head = system_wq->tail = NULL;
    spinlock_init(&system_wq->lock.lock);
    simple_strcpy(system_wq->name, "events");
    
    system_long_wq->head = system_long_wq->tail = NULL;
    spinlock_init(&system_long_wq->lock.lock);
    simple_strcpy(system_long_wq->name, "events_long");
    
    timerwheel_init();
    
    screen_log("OK", COLOR_LIGHT_GREEN, "Workqueue subsystem initialized");
}

struct workqueue_struct *alloc_workqueue(const char *name, uint32_t flags) {
    (void)flags;
    struct workqueue_struct *wq = (struct workqueue_struct *)kmalloc(sizeof(struct workqueue_struct));
    if (!wq) return NULL;
    
    wq->head = wq->tail = NULL;
    spinlock_init(&wq->lock.lock);
    simple_strcpy(wq->name, name);
    return wq;
}

void destroy_workqueue(struct workqueue_struct *wq) {
    if (!wq) return;
    
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, &flags);
    
    /* Cancel all pending work */
    struct work_struct *work = wq->head;
    while (work) {
        struct work_struct *next = work->next;
        work->next = NULL;
        work = next;
    }
    wq->head = wq->tail = NULL;
    
    spin_unlock_irqrestore(&wq->lock, flags);
    kfree(wq);
}

int queue_work(struct workqueue_struct *wq, struct work_struct *work) {
    if (!wq || !work) return -1;
    
    work->delayed = 0;
    work->execute_at = 0;
    
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, &flags);
    
    work->next = NULL;
    if (wq->tail) {
        wq->tail->next = work;
        wq->tail = work;
    } else {
        wq->head = wq->tail = work;
    }
    
    spin_unlock_irqrestore(&wq->lock, flags);
    return 0;
}

int queue_delayed_work(struct workqueue_struct *wq, struct work_struct *work, uint64_t delay_ticks) {
    if (!wq || !work || delay_ticks == 0) return queue_work(wq, work);
    
    work->delayed = 1;
    work->execute_at = timer_get_ticks() + delay_ticks;
    
    timerwheel_add_delayed_work(work, delay_ticks);
    return 0;
}

int cancel_work_sync(struct work_struct *work) {
    if (!work || !work->delayed) return -1;
    
    /* For delayed work, we need to remove from timerwheel */
    /* Simplified: just mark as cancelled */
    work->func = NULL;
    return 0;
}

int cancel_delayed_work_sync(struct work_struct *work) {
    return cancel_work_sync(work);
}

void flush_workqueue(struct workqueue_struct *wq) {
    if (!wq) return;
    
    while (1) {
        unsigned long flags;
        spin_lock_irqsave(&wq->lock, &flags);
        struct work_struct *work = wq->head;
        if (!work) {
            spin_unlock_irqrestore(&wq->lock, flags);
            break;
        }
        wq->head = work->next;
        if (!wq->head) wq->tail = NULL;
        work->next = NULL;
        spin_unlock_irqrestore(&wq->lock, flags);
        
        if (work->func) work->func(work);
    }
}

void timerwheel_init(void) {
    for (int i = 0; i < TIMERWHEEL_SIZE; i++) {
        timerwheel[i].head = timerwheel[i].tail = NULL;
        spinlock_init(&timerwheel[i].lock.lock);
    }
    timerwheel_current = timer_get_ticks();
}

void timerwheel_add_delayed_work(struct work_struct *work, uint64_t delay_ticks) {
    if (!work || delay_ticks == 0) return;
    
    uint64_t execute_at = timer_get_ticks() + delay_ticks;
    work->execute_at = execute_at;
    
    int bucket_idx = execute_at & (TIMERWHEEL_SIZE - 1);
    struct timerwheel_bucket *bucket = &timerwheel[bucket_idx];
    
    unsigned long flags;
    spin_lock_irqsave(&bucket->lock, &flags);
    
    work->next = NULL;
    if (bucket->tail) {
        bucket->tail->next = work;
        bucket->tail = work;
    } else {
        bucket->head = bucket->tail = work;
    }
    
    spin_unlock_irqrestore(&bucket->lock, flags);
}

/* Forward declaration */
void timerwheel_register_timer(void);

void timerwheel_process(uint64_t current_tick) {
    int bucket_idx = current_tick & (TIMERWHEEL_SIZE - 1);
    struct timerwheel_bucket *bucket = &timerwheel[bucket_idx];
    
    unsigned long flags;
    spin_lock_irqsave(&bucket->lock, &flags);
    
    struct work_struct *work = bucket->head;
    bucket->head = bucket->tail = NULL;
    
    spin_unlock_irqrestore(&bucket->lock, flags);
    
    while (work) {
        struct work_struct *next = work->next;
        work->next = NULL;
        work->delayed = 0;
        
        if (work->func && work->execute_at <= current_tick) {
            /* Queue to system workqueue for execution */
            queue_work(system_wq, work);
        } else if (work->execute_at > current_tick) {
            /* Re-queue for later */
            timerwheel_add_delayed_work(work, work->execute_at - current_tick);
        }
        work = next;
    }
}

/* Timer callback to process timerwheel */
static void timerwheel_timer_callback(uint64_t data) {
    (void)data;
    timerwheel_process(timer_get_ticks());
    
    /* Re-arm the timer for next tick */
    timerwheel_register_timer();
}

void timerwheel_register_timer(void) {
    /* Register a 1-tick timer to process the wheel */
    timer_entry_t timer;
    timer.expires = timer_get_ticks() + 1;
    timer.function = timerwheel_timer_callback;
    timer.data = 0;
    timer_add(&timer);
}