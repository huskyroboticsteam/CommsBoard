/**
  * dbg_uart.h - debug text out over USART2 (the ST-LINK virtual COM port on the Nucleo).
  * Text goes into a buffer and is sent by DMA in the background, so printing never
  * blocks. If the buffer is full, the extra text is thrown away.
  */
#ifndef DBG_UART_H
#define DBG_UART_H

#include <stddef.h>

void dbg_init(void);
void dbg_write(const char *text, size_t len);
void dbg_puts(const char *text);     /* printf() also ends up here, through __io_putchar */

#endif /* DBG_UART_H */
