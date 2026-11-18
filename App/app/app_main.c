/* app_main.c - start-up glue between the CubeMX-generated code and the application. */
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
}

void app_fatal(void)
{
  board_fatal();
}
