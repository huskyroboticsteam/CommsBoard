/**
  * status.c - heartbeat LED, fault LED, watchdog, and the stats line on the debug UART.
  *
  * Watchdog: the firmware "checks in" whenever it makes progress: the main loop finished
  * a pass, or the SD card answered a command. Every 100 ms this job refreshes the
  * hardware watchdog only if there was a check-in since the last refresh. If the main
  * loop hangs, the check-ins stop (or this job stops being called), and the MCU resets
  * after about 2 s.
  */
#include "app_main.h"
#include "board.h"
#include "config.h"
#include "can_drv.h"
#include "dbg_uart.h"
#include "sd_spi.h"
#include "timebase.h"
#include <stdio.h>

#define TICK_MS 100

static uint8_t checked_in;

void wdg_checkin(void)
{
  checked_in = 1;
}

static const char *const STATE_NAME[] = { "INIT", "OPEN", "LOGGING", "FAULT", "CARD_FULL" };
static const char *const CAN_STATE[] = { "ok", "warn", "passive", "busoff" };

/* One line every STATS_PRINT_MS. Rates are averages since the previous line. */
static void print_stats(void)
{
  static uint32_t last_rx, last_tx, last_writes;
  static uint64_t last_bytes, last_sum_us;
  static char line[280];
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
               "[%lus] %s b%lu p%lu | rx %lu/s tx %lu/s drop %lu+%lu txfail %lu | ring hi %lu/%lu | "
               "sd avg %luus max %luus err %lu poll %lu | json %lu B/s | can %s tec %u rec %u | "
               "gap %luus | stack free %lu\r\n",
               (unsigned long)(timebase_now_us() / 1000000u), STATE_NAME[lg.state],
               (unsigned long)lg.boot_id, (unsigned long)lg.part,
               (unsigned long)((can.rx_frames - last_rx) / secs), (unsigned long)((can.tx_frames - last_tx) / secs),
               (unsigned long)can.rx_dropped, (unsigned long)ct.log_dropped,
               (unsigned long)(ct.tx_queue_full + ct.tx_refused),
               (unsigned long)log_ring.high_water, (unsigned long)(LOG_RING_RECORDS - 1),
               (unsigned long)avg_us, (unsigned long)sd.write_max_us, (unsigned long)sd.errors,
               (unsigned long)sd.polled_blocks,    /* blocks that could not go by DMA: should stay 0 */
               (unsigned long)((lg.bytes - last_bytes) / secs),
               CAN_STATE[can.state & 3], can.tec, can.rec,
               (unsigned long)app_can_gap_max_us(),   /* longest wait between CAN job runs since the last line */
               (unsigned long)board_stack_free());   /* bytes of stack never used so far */
  if (n > 0) dbg_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);

  last_rx = can.rx_frames;
  last_tx = can.tx_frames;
  last_writes = sd.writes;
  last_sum_us = sd.write_sum_us;
  last_bytes = lg.bytes;
}

void status_poll(void)
{
  static uint32_t last_ms, ticks;
  logger_stats_t lg;

  if (timebase_ms() - last_ms < TICK_MS) return;
  last_ms += TICK_MS;
  if (timebase_ms() - last_ms >= TICK_MS) last_ms = timebase_ms();    /* fell behind: do not burst */
  ticks++;

  if (ticks % (500 / TICK_MS) == 0) led_toggle(LED_HB);         /* 1 Hz blink = firmware alive */

  /* Fault LED: solid while nothing is being logged. (Chapter 6 adds the blink codes.) */
  logger_get_stats(&lg);
  led_set(LED_FAULT, lg.state == LOGGER_FAULT || lg.state == LOGGER_CARD_FULL);

  if (ticks % (STATS_PRINT_MS / TICK_MS) == 0) print_stats();

  if (checked_in) {
    checked_in = 0;
    board_watchdog_kick();
  }
}
