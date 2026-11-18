/* timekeeping.c - time arithmetic. See timekeeping.h. */
#include "timekeeping.h"

uint64_t tk_extend_us(uint64_t now_us, uint32_t t_lo)
{
  /* How long ago the record was stamped, modulo 2^32 us. */
  uint32_t age = (uint32_t)now_us - t_lo;
  return now_us - age;
}

/* Days from 1970-01-01 to the given date (Howard Hinnant's days_from_civil). */
static uint32_t days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
  uint32_t era, yoe, doy, doe;

  y -= m <= 2;
  era = y / 400;
  yoe = y - era * 400;
  doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

uint32_t tk_civil_to_unix(const rtc_civil_t *c)
{
  return days_from_civil(c->year, c->month, c->day) * 86400u
       + c->hour * 3600u + c->minute * 60u + c->second;
}

void tk_unix_to_civil(uint32_t unix_s, rtc_civil_t *c)
{
  uint32_t z = unix_s / 86400u + 719468;
  uint32_t rem = unix_s % 86400u;
  uint32_t era = z / 146097;
  uint32_t doe = z - era * 146097;
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;

  c->year = (uint16_t)(yoe + era * 400 + (m <= 2));
  c->month = (uint8_t)m;
  c->day = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
  c->hour = (uint8_t)(rem / 3600);
  c->minute = (uint8_t)(rem % 3600 / 60);
  c->second = (uint8_t)(rem % 60);
}

uint32_t tk_rtc_unix(void)
{
  rtc_civil_t c;

  timebase_rtc_read(&c);
  return tk_civil_to_unix(&c);
}

void tk_rtc_set_unix(uint32_t unix_s)
{
  rtc_civil_t c;

  tk_unix_to_civil(unix_s, &c);
  timebase_rtc_write(&c);
}

uint32_t tk_fattime(void)
{
  rtc_civil_t c = { 2026, 1, 1, 0, 0, 0 };    /* shown on files written before the time is set */

  if (timebase_rtc_is_valid()) timebase_rtc_read(&c);
  /* FatFs layout: year-1980 <<25 | month <<21 | day <<16 | hour <<11 | minute <<5 | second/2 */
  return ((uint32_t)(c.year - 1980) << 25) | ((uint32_t)c.month << 21) | ((uint32_t)c.day << 16)
       | ((uint32_t)c.hour << 11) | ((uint32_t)c.minute << 5) | (c.second / 2u);
}
