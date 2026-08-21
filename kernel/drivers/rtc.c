#include <rtc.h>
#include <idt.h>
#include <io.h>
#include <task.h>
#include <spinlock.h>
#include <workqueue.h>
#include <serial.h>

static uint64_t rtc_epoch_seconds = 0;
static uint64_t rtc_nanoseconds = 0;
static spinlock_irq_t rtc_lock = { SPINLOCK_INIT, 0 };
static bool rtc_initialized = false;

struct work_struct rtc_work;

static inline uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
}

static inline void cmos_write(uint8_t reg, uint8_t val) {
    outb(0x70, reg);
    io_wait();
    outb(0x71, val);
    io_wait();
}

static inline uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

static inline bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static inline int days_in_month(int month, int year) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) return 29;
    return days[month - 1];
}

static uint64_t rtc_read_epoch(void) {
    uint8_t sec, min, hour, day, month, century, year;
    uint8_t reg_b;

    do {
        sec = cmos_read(0x00);
        min = cmos_read(0x02);
        hour = cmos_read(0x04);
        day = cmos_read(0x07);
        month = cmos_read(0x08);
        year = cmos_read(0x09);
        century = cmos_read(0x32);
    } while (sec != cmos_read(0x00));

    reg_b = cmos_read(0x0B);

    if (!(reg_b & 0x04)) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    if (!(reg_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    int full_year = century * 100 + year;

    uint64_t days = 0;
    for (int y = 1970; y < full_year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    for (int m = 1; m < month; m++) {
        days += days_in_month(m, full_year);
    }
    days += day - 1;

    return days * 86400ULL + hour * 3600ULL + min * 60ULL + sec;
}

static void rtc_work_handler(struct work_struct *work) {
    (void)work;
    unsigned long flags;
    spin_lock_irqsave(&rtc_lock, &flags);
    rtc_epoch_seconds = rtc_read_epoch();
    spin_unlock_irqrestore(&rtc_lock, flags);
}

static void rtc_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    cmos_read(0x0C);
    queue_work(system_wq, &rtc_work);
}

void rtc_init(void) {
    unsigned long flags;
    spin_lock_irqsave(&rtc_lock, &flags);

    uint8_t reg_b = cmos_read(0x0B);
    reg_b |= 0x40;
    cmos_write(0x0B, reg_b);

    rtc_epoch_seconds = rtc_read_epoch();
    rtc_nanoseconds = 0;

    INIT_WORK(&rtc_work, rtc_work_handler);
    interrupts_register_handler(40, rtc_irq_handler);

    uint8_t mask = inb(0xA1);
    outb(0xA1, mask & ~0x01);

    rtc_initialized = true;
    spin_unlock_irqrestore(&rtc_lock, flags);

    serial_print("ApolloOS: RTC initialized\n");
}

uint64_t rtc_get_epoch_seconds(void) {
    unsigned long flags;
    spin_lock_irqsave(&rtc_lock, &flags);
    uint64_t sec = rtc_epoch_seconds;
    spin_unlock_irqrestore(&rtc_lock, flags);
    return sec;
}

uint64_t rtc_get_epoch_nanoseconds(void) {
    unsigned long flags;
    spin_lock_irqsave(&rtc_lock, &flags);
    uint64_t sec = rtc_epoch_seconds;
    uint64_t ns = rtc_nanoseconds;
    spin_unlock_irqrestore(&rtc_lock, flags);
    return sec * 1000000000ULL + ns;
}

void rtc_update_nanoseconds(uint64_t ns) {
    unsigned long flags;
    spin_lock_irqsave(&rtc_lock, &flags);
    rtc_nanoseconds = ns;
    spin_unlock_irqrestore(&rtc_lock, flags);
}

bool rtc_is_initialized(void) {
    return rtc_initialized;
}