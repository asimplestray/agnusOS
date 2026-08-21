#include <timer.h>
#include <idt.h>
#include <io.h>
#include <task.h>
#include <spinlock.h>
#include <stddef.h>
#include <rtc.h>
#include <bcache.h>

static uint64_t timer_ticks = 0;
extern volatile uint64_t need_resched;

/* Dynamic timer list and interrupt lock */
static timer_entry_t *timer_list = NULL;
static spinlock_irq_t timer_lock = { SPINLOCK_INIT, 0 };

/* Periodic write-back: flush dirty disk sectors every 2 seconds so a
 * crash/reboot loses at most a couple of seconds of FAT32 data. */
#define FLUSH_INTERVAL_TICKS 200

static void timer_callback(struct interrupt_frame* frame) {
    (void)frame;
    timer_ticks++;

    rtc_update_nanoseconds((timer_ticks % 100) * 10000000ULL);

    /* Periodic dirty-page flush (every 2s at 100Hz) */
    if ((timer_ticks % FLUSH_INTERVAL_TICKS) == 0) {
        bcache_flush();
    }

    /* Process all expired timers */
    /* Note: Since we are already inside the IRQ handler, interrupts are disabled.
     * We only grab the lock to synchronize against other cores/threads. */
    spin_lock(&timer_lock.lock);
    while (timer_list && timer_list->expires <= timer_ticks) {
        timer_entry_t *t = timer_list;
        timer_list = timer_list->next;
        
        if (t->function) {
            /* Release the lock before calling the handler to prevent deadlock
             * if the handler tries to modify timers. */
            spin_unlock(&timer_lock.lock);
            t->function(t->data);
            spin_lock(&timer_lock.lock);
        }
    }
    spin_unlock(&timer_lock.lock);

    if (current) {
        current->ticks++;
        current->counter--;

        if (current->counter <= 0) {
            need_resched = 1;
        }
    }
}

void timer_init(uint32_t frequency) {
    interrupts_register_handler(32, timer_callback);
    uint32_t divisor = 1193180 / frequency;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    uint8_t mask = inb(0x21);
    outb(0x21, mask & ~0x01);
}

uint64_t timer_get_ticks(void) {
    return timer_ticks;
}

void timer_add(timer_entry_t *timer) {
    if (!timer) return;
    unsigned long flags;
    spin_lock_irqsave(&timer_lock, &flags);

    timer->next = NULL;
    if (!timer_list || timer->expires < timer_list->expires) {
        timer->next = timer_list;
        timer_list = timer;
    } else {
        timer_entry_t *curr = timer_list;
        while (curr->next && curr->next->expires <= timer->expires) {
            curr = curr->next;
        }
        timer->next = curr->next;
        curr->next = timer;
    }

    spin_unlock_irqrestore(&timer_lock, flags);
}

void timer_remove(timer_entry_t *timer) {
    if (!timer) return;
    unsigned long flags;
    spin_lock_irqsave(&timer_lock, &flags);

    if (timer_list == timer) {
        timer_list = timer_list->next;
    } else {
        timer_entry_t *curr = timer_list;
        while (curr && curr->next != timer) {
            curr = curr->next;
        }
        if (curr) {
            curr->next = timer->next;
        }
    }

    spin_unlock_irqrestore(&timer_lock, flags);
}
