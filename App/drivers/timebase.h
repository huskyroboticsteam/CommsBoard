/**
  * timebase.h - the two clocks:
  *  - a microsecond clock: TIM2 free-running at 1 MHz, extended to 64 bits;
  *  - the RTC (wall-clock time), plus the backup registers that survive a reset.
  *
  * This header has no HAL types, so services can include it and be faked in PC tests.
  */
#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

void timebase_init(void);            /* start TIM2 */
uint64_t timebase_now_us(void);      /* microseconds since boot; safe in tasks and interrupts */
uint32_t timebase_now_lo(void);      /* low 32 bits only: one register read, for interrupts */
void timebase_on_overflow(void);     /* call from the TIM2 period-elapsed callback */

/* Calendar time as the RTC holds it (UTC, years 2000-2099). */
typedef struct { uint16_t year; uint8_t month, day, hour, minute, second; } rtc_civil_t;

int  timebase_rtc_is_valid(void);    /* has the time been set since the RTC last lost power? */
void timebase_rtc_read(rtc_civil_t *c);
void timebase_rtc_write(const rtc_civil_t *c);   /* also marks the time valid */

uint32_t timebase_boot_id_get(void); /* last boot ID, kept in a backup register */
void timebase_boot_id_set(uint32_t id);

#endif /* TIMEBASE_H */
