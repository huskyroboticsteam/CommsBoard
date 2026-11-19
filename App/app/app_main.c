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

void app_loop(void)
{
  can_task_poll();
  logger_poll();      /* the only job that can take long: it calls app_yield() while the card is busy */
  status_poll();
  wdg_checkin();      /* a whole pass finished */
}

void app_yield(void)
{
  can_task_poll();
  status_poll();
}

void app_fatal(void)
{
  board_fatal();
}
