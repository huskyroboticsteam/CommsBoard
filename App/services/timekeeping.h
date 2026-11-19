/**
  * timekeeping.h - time arithmetic on top of the clocks in drivers/timebase.
  * The maths has no hardware in it, so the PC unit tests run it with a fake RTC.
  */
#ifndef TIMEKEEPING_H
#define TIMEKEEPING_H

#include <stdint.h>
#include "timebase.h"

/* Rebuild a full 64-bit microsecond time from a record's low 32 bits.
 * now_us is roughly the current 64-bit time; the record may be up to ~35 minutes
 * older or newer than it. */
uint64_t tk_extend_us(uint64_t now_us, uint32_t t_lo);

/* Calendar date/time (UTC) <-> Unix seconds, valid for years 2000-2099 (the RTC's range). */
uint32_t tk_civil_to_unix(const rtc_civil_t *c);
void tk_unix_to_civil(uint32_t unix_s, rtc_civil_t *c);

uint32_t tk_rtc_unix(void);             /* RTC time as Unix seconds (meaningless if not valid) */
void tk_rtc_set_unix(uint32_t unix_s);  /* set the RTC and mark it valid */
uint32_t tk_fattime(void);              /* RTC time packed the way FatFs stamps files */

#endif /* TIMEKEEPING_H */
