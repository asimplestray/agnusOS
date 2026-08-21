#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Dynamic timer structure */
typedef struct timer_entry {
    uint64_t expires;             /* Clock ticks when this timer expires */
    void (*function)(uint64_t);   /* Callback function to invoke */
    uint64_t data;                /* Arbitrary data passed to the callback */
    struct timer_entry *next;     /* Link to next timer in queue */
} timer_entry_t;

/* Initialize system timer (PIT) at target frequency (e.g. 100Hz) */
void timer_init(uint32_t frequency);

/* Get current system ticks since boot */
uint64_t timer_get_ticks(void);

/* Dynamic timer registration/unregistration */
void timer_add(timer_entry_t *timer);
void timer_remove(timer_entry_t *timer);

#endif
