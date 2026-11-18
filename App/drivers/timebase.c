/* timebase.c - microsecond clock (TIM2) and RTC access. See timebase.h. */
#include "timebase.h"
#include "board.h"
#include "tim.h"
#include "rtc.h"

#define RTC_VALID_MAGIC   0x32F2u
#define BKP_REG_VALID     RTC_BKP_DR0
#define BKP_REG_BOOT_ID   RTC_BKP_DR1

static volatile uint32_t overflows;   /* times TIM2 has wrapped: the upper 32 bits */

void timebase_init(void)
{
  HAL_TIM_Base_Start_IT(&BOARD_US_TIMER_HANDLE);
}

void timebase_on_overflow(void)
{
  overflows++;
}

uint32_t timebase_now_lo(void)
{
  return BOARD_US_TIMER_HANDLE.Instance->CNT;
}

uint64_t timebase_now_us(void)
{
  TIM_TypeDef *tim = BOARD_US_TIMER_HANDLE.Instance;
  uint32_t hi, lo, wrapped;

  do {                              /* upper, lower, upper again: retry if a wrap slipped in between */
    hi = overflows;
    lo = tim->CNT;
    wrapped = tim->SR & TIM_SR_UIF;
  } while (hi != overflows);

  /* The counter wrapped but its interrupt has not run yet (we are in a more urgent
   * interrupt, or interrupts are masked): count that wrap ourselves. */
  if (wrapped && lo < 0x80000000u) hi++;
  return ((uint64_t)hi << 32) | lo;
}

int timebase_rtc_is_valid(void)
{
  return HAL_RTCEx_BKUPRead(&hrtc, BKP_REG_VALID) == RTC_VALID_MAGIC;
}

void timebase_rtc_read(rtc_civil_t *c)
{
  RTC_TimeTypeDef t;
  RTC_DateTypeDef d;

  HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);   /* must follow GetTime: it unlocks the registers */
  c->year = (uint16_t)(2000 + d.Year);
  c->month = d.Month;
  c->day = d.Date;
  c->hour = t.Hours;
  c->minute = t.Minutes;
  c->second = t.Seconds;
}

void timebase_rtc_write(const rtc_civil_t *c)
{
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0};

  t.Hours = c->hour;
  t.Minutes = c->minute;
  t.Seconds = c->second;
  d.Year = (uint8_t)(c->year - 2000);
  d.Month = c->month;
  d.Date = c->day;
  d.WeekDay = RTC_WEEKDAY_MONDAY;     /* not used */
  HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN);
  HAL_RTCEx_BKUPWrite(&hrtc, BKP_REG_VALID, RTC_VALID_MAGIC);
}

uint32_t timebase_boot_id_get(void)
{
  return HAL_RTCEx_BKUPRead(&hrtc, BKP_REG_BOOT_ID);
}

void timebase_boot_id_set(uint32_t id)
{
  HAL_RTCEx_BKUPWrite(&hrtc, BKP_REG_BOOT_ID, id);
}
