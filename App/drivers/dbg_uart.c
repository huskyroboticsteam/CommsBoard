/* dbg_uart.c - buffered, non-blocking debug output over USART2 with DMA. See dbg_uart.h. */
#include "dbg_uart.h"
#include "board.h"
#include "usart.h"
#include <string.h>

#define DBG_BUF_SIZE 1024

static uint8_t buf[DBG_BUF_SIZE];
static volatile uint16_t head;      /* next byte to fill */
static volatile uint16_t tail;      /* next byte to send */
static volatile uint16_t sending;   /* bytes in the DMA transfer under way (0 = idle) */

/* Start sending the next run of buffered bytes. Call with interrupts masked. */
static void kick(void)
{
  uint16_t n;

  if (sending || head == tail) return;
  n = (head > tail) ? (uint16_t)(head - tail) : (uint16_t)(DBG_BUF_SIZE - tail);   /* up to the wrap */
  if (HAL_UART_Transmit_DMA(&BOARD_DBG_UART_HANDLE, &buf[tail], n) == HAL_OK) sending = n;
}

void dbg_init(void)
{
  head = tail = sending = 0;
}

void dbg_write(const char *text, size_t len)
{
  uint32_t primask = __get_PRIMASK();
  size_t i;

  __disable_irq();                  /* the DMA-complete interrupt uses these too; keep this short */
  for (i = 0; i < len; i++) {
    uint16_t next = (uint16_t)((head + 1) % DBG_BUF_SIZE);
    if (next == tail) break;        /* full: drop the rest */
    buf[head] = (uint8_t)text[i];
    head = next;
  }
  kick();
  __set_PRIMASK(primask);
}

void dbg_puts(const char *text)
{
  dbg_write(text, strlen(text));
}

int __io_putchar(int ch)
{
  char c = (char)ch;
  dbg_write(&c, 1);
  return ch;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart != &BOARD_DBG_UART_HANDLE) return;
  tail = (uint16_t)((tail + sending) % DBG_BUF_SIZE);
  sending = 0;
  kick();
}
