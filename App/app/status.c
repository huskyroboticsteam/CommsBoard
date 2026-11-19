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
#include "fmt.h"
#include "sd_spi.h"
#include "timebase.h"

#define TICK_MS 100

static uint8_t checked_in;

void wdg_checkin(void)
{
  checked_in = 1;
}

static const char *const STATE_NAME[] = { "INIT", "OPEN", "LOGGING", "FAULT", "CARD_FULL" };
static const char *const CAN_STATE[] = { "ok", "warn", "passive", "busoff" };

/* One line every STATS_PRINT_MS. Rates are averages since the previous line.
 * Built with fmt.h, not snprintf, so that malloc stays out of the firmware. */
static void print_stats(void)
{
  static uint32_t last_rx, last_tx, last_writes;
  static uint64_t last_bytes, last_sum_us;
  static char line[400];      /* worst case is about 365 characters: 21 numbers of up to 10 digits */
  can_stats_t can;
  can_task_stats_t ct;
  logger_stats_t lg;
  SD_Stats sd = *SD_GetStats();   /* read-only, so allowed here: see the rule on app_yield() */
  uint32_t secs = STATS_PRINT_MS / 1000, writes, avg_us;
  char *p = line;

  can_drv_get_stats(&can);
  can_task_get_stats(&ct);
  logger_get_stats(&lg);
  writes = sd.writes - last_writes;
  avg_us = writes ? (uint32_t)((sd.write_sum_us - last_sum_us) / writes) : 0;

  p = fmt_str(p, "[");            p = fmt_u32(p, (uint32_t)(timebase_now_us() / 1000000u));
  p = fmt_str(p, "s] ");          p = fmt_str(p, STATE_NAME[lg.state]);
  p = fmt_str(p, " b");           p = fmt_u32(p, lg.boot_id);
  p = fmt_str(p, " p");           p = fmt_u32(p, lg.part);
  p = fmt_str(p, " | rx ");       p = fmt_u32(p, (can.rx_frames - last_rx) / secs);
  p = fmt_str(p, "/s tx ");       p = fmt_u32(p, (can.tx_frames - last_tx) / secs);
  p = fmt_str(p, "/s drop ");     p = fmt_u32(p, can.rx_dropped);       /* lost in the interrupt */
  p = fmt_str(p, "+");            p = fmt_u32(p, ct.log_dropped);       /* lost because the log ring was full */
  p = fmt_str(p, " txfail ");     p = fmt_u32(p, ct.tx_queue_full + ct.tx_refused);
  p = fmt_str(p, " | ring hi ");  p = fmt_u32(p, log_ring.high_water);
  p = fmt_str(p, "/");            p = fmt_u32(p, LOG_RING_RECORDS - 1);
  p = fmt_str(p, " | sd avg ");   p = fmt_u32(p, avg_us);
  p = fmt_str(p, "us max ");      p = fmt_u32(p, sd.write_max_us);
  p = fmt_str(p, "us err ");      p = fmt_u32(p, sd.errors);
  p = fmt_str(p, " poll ");       p = fmt_u32(p, sd.polled_blocks);     /* blocks that could not go by DMA: should stay 0 */
  p = fmt_str(p, " | json ");     p = fmt_u32(p, (uint32_t)((lg.bytes - last_bytes) / secs));
  p = fmt_str(p, " B/s | can ");  p = fmt_str(p, CAN_STATE[can.state & 3]);
  p = fmt_str(p, " tec ");        p = fmt_u32(p, can.tec);
  p = fmt_str(p, " rec ");        p = fmt_u32(p, can.rec);
  p = fmt_str(p, " | gap ");      p = fmt_u32(p, app_can_gap_max_us());  /* longest wait between CAN job runs since the last line */
  p = fmt_str(p, "us | stack free "); p = fmt_u32(p, board_stack_free()); /* bytes of stack never used so far */
  p = fmt_str(p, "\r\n");
  dbg_write(line, (size_t)(p - line));

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
