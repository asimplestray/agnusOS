#ifndef RTC_H
#define RTC_H

#include <stdint.h>

void rtc_init(void);
uint64_t rtc_get_epoch_seconds(void);
uint64_t rtc_get_epoch_nanoseconds(void);
void rtc_update_nanoseconds(uint64_t ns);
bool rtc_is_initialized(void);

#endif