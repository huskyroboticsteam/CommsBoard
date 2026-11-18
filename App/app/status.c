/**
  * status.c - heartbeat LED, fault LED, watchdog, and the stats line on the debug UART.
  *
  * Watchdog: every task sets its bit each time round its loop. This task refreshes the
  * hardware watchdog only when all bits are set, then clears them. A task that hangs
  * stops setting its bit, the refresh stops, and the MCU resets after about 2 s.
  */
#include "app_main.h"
#include "board.h"
#include "config.h"
#include "can_drv.h"
#include "dbg_uart.h"
#include "sd_spi.h"
#include "timebase.h"
#include "cmsis_os.h"
#include <stdio.h>

#define TICK_MS 100

static uint32_t checkins;

void wdg_checkin(uint32_t task_bit)
{
  __atomic_fetch_or(&checkins, task_bit, __ATOMIC_RELAXED);
}

static const char *const STATE_NAME[] = { "INIT", "OPEN", "LOGGING", "FAULT", "CARD_FULL" };
static const char *const CAN_STATE[] = { "ok", "warn", "passive", "busoff" };

/* One line every STATS_PRINT_MS. Rates are averages since the previous line. */
static void print_stats(void)
{
  static uint32_t last_rx, last_tx, last_writes;
  static uint64_t last_bytes, last_sum_us;
  static char line[200];      /* static: keeps this task's 1 KB stack for snprintf */
  can_stats_t can;
  can_task_stats_t ct;
  logger_stats_t lg;
  SD_Stats sd = *SD_GetStats();
  uint32_t secs = STATS_PRINT_MS / 1000, writes, avg_us;
  int n;

  can_drv_get_stats(&can);
  can_task_get_stats(&ct);
  logger_get_stats(&lg);
  writes = sd.writes - last_writes;
  avg_us = writes ? (uint32_t)((sd.write_sum_us - last_sum_us) / writes) : 0;

  n = snprintf(line, sizeof line,
               "[%lus] %s b%lu p%lu | rx %lu/s tx %lu/s drop %lu+%lu | ring hi %lu/%lu | "
               "sd avg %luus max %luus err %lu | json %lu B/s | can %s tec %u rec %u\r\n",
               (unsigned long)(timebase_now_us() / 1000000u), STATE_NAME[lg.state],
               (unsigned long)lg.boot_id, (unsigned long)lg.part,
               (unsigned long)((can.rx_frames - last_rx) / secs), (unsigned long)((can.tx_frames - last_tx) / secs),
               (unsigned long)can.rx_dropped, (unsigned long)ct.log_dropped,
               (unsigned long)log_ring.high_water, (unsigned long)(LOG_RING_RECORDS - 1),
               (unsigned long)avg_us, (unsigned long)sd.write_max_us, (unsigned long)sd.errors,
               (unsigned long)((lg.bytes - last_bytes) / secs),
               CAN_STATE[can.state & 3], can.tec, can.rec);
  if (n > 0) dbg_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);

  last_rx = can.rx_frames;
  last_tx = can.tx_frames;
  last_writes = sd.writes;
  last_sum_us = sd.write_sum_us;
  last_bytes = lg.bytes;
}

void status_task(void *argument)
{
  logger_stats_t lg;
  uint32_t ticks = 0;

  (void)argument;
  for (;;) {
    osDelay(TICK_MS);
    ticks++;

    if (ticks % (500 / TICK_MS) == 0) led_toggle(LED_HB);       /* 1 Hz blink = firmware alive */

    /* Fault LED: solid while nothing is being logged. (Chapter 6 adds the blink codes.) */
    logger_get_stats(&lg);
    led_set(LED_FAULT, lg.state == LOGGER_FAULT || lg.state == LOGGER_CARD_FULL);

    if (ticks % (STATS_PRINT_MS / TICK_MS) == 0) print_stats();

    wdg_checkin(WDG_STATUS);
    if ((__atomic_load_n(&checkins, __ATOMIC_RELAXED) & WDG_ALL) == WDG_ALL) {
      __atomic_store_n(&checkins, 0, __ATOMIC_RELAXED);
      board_watchdog_kick();
    }
  }
}
