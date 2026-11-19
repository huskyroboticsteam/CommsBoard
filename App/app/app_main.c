/* app_main.c - start-up and the main loop. See app_main.h for how the jobs share the CPU. */
#include "app_main.h"
#include "board.h"
#include "config.h"
#include "dbg_uart.h"
#include "timebase.h"

void app_main(void)
{
  board_init();
  timebase_init();
  dbg_init();
  dbg_puts("\r\nComms Board firmware (" BOARD_NAME "), reset: ");
  dbg_puts(board_reset_cause());
  dbg_puts("\r\n");
  can_task_init();
  logger_init();
}

static uint32_t can_gap_max_us;

/* The CAN job, with a stopwatch: how long since it last ran? */
static void run_can(void)
{
  static uint32_t last_us;
  static uint8_t started;
  uint32_t now = timebase_now_lo();

  if (started && now - last_us > can_gap_max_us) can_gap_max_us = now - last_us;
  started = 1;
  last_us = now;
  can_task_poll();
}

uint32_t app_can_gap_max_us(void)
{
  uint32_t gap = can_gap_max_us;

  can_gap_max_us = 0;
  return gap;
}

void app_loop(void)
{
  run_can();
  logger_poll();      /* the only job that can take long: it calls app_yield() while the card is busy */
  status_poll();
  wdg_checkin();      /* a whole pass finished */
}

/* See the rule in app_main.h: nothing here may reach FatFs, storage or the SD driver. */
void app_yield(void)
{
  static uint8_t yielding;

  if (yielding) return;     /* never nest, whatever a job ends up calling */
  yielding = 1;
  run_can();
  status_poll();
  yielding = 0;
}

void app_fatal(void)
{
  board_fatal();
}
