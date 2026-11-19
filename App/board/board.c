/* board.c - board bring-up that CubeMX does not generate. See board.h. */
#include "board.h"
#include "iwdg.h"

#define STACK_FILL 0xA5u

/* From the linker script: the end of our variables, and the space reserved for a heap.
 * Above those, everything up to the top of RAM is stack, which grows downwards. */
extern uint8_t _end, _Min_Heap_Size;
#define STACK_BOTTOM (&_end + (uintptr_t)&_Min_Heap_Size)

static const char *reset_cause = "?";

void board_init(void)
{
  uint32_t csr = RCC->CSR;
  uint8_t *p, *sp = (uint8_t *)__get_MSP() - 64;

  /* Paint the unused stack so board_stack_free() can tell how deep it ever got. */
  for (p = STACK_BOTTOM; p < sp; p++) *p = STACK_FILL;

  /* Why did we reset? Most specific cause first; then clear the flags for next time. */
  if (csr & RCC_CSR_IWDGRSTF)      reset_cause = "IWDG";
  else if (csr & RCC_CSR_WWDGRSTF) reset_cause = "WWDG";
  else if (csr & RCC_CSR_SFTRSTF)  reset_cause = "SW";
  else if (csr & RCC_CSR_LPWRRSTF) reset_cause = "LPWR";
  else if (csr & RCC_CSR_OBLRSTF)  reset_cause = "OBL";
  else if (csr & RCC_CSR_BORRSTF)  reset_cause = "POR";
  else if (csr & RCC_CSR_PINRSTF)  reset_cause = "PIN";
  __HAL_RCC_CLEAR_RESET_FLAGS();

  /* Let the watchdog pause while the CPU is halted in the debugger. */
  __HAL_DBGMCU_FREEZE_IWDG();

  HAL_NVIC_SetPriority(COMP1_2_3_IRQn,     BOARD_IRQ_PRIO_POWERFAIL, 0);
  HAL_NVIC_SetPriority(FDCAN2_IT0_IRQn,    BOARD_IRQ_PRIO_CAN, 0);
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, BOARD_IRQ_PRIO_SD_DMA, 0);   /* SPI1 RX */
  HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, BOARD_IRQ_PRIO_SD_DMA, 0);   /* SPI1 TX */
  HAL_NVIC_SetPriority(TIM2_IRQn,          BOARD_IRQ_PRIO_US_TIMER, 0);
  HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, BOARD_IRQ_PRIO_DBG_UART, 0); /* USART2 TX */
  HAL_NVIC_SetPriority(USART2_IRQn,        BOARD_IRQ_PRIO_DBG_UART, 0);
}

uint32_t board_stack_free(void)
{
  const uint8_t *p = STACK_BOTTOM;

  while (*p == STACK_FILL) p++;     /* stops at the deepest byte the stack has touched */
  return (uint32_t)(p - STACK_BOTTOM);
}

const char *board_reset_cause(void)
{
  return reset_cause;
}

void board_watchdog_kick(void)
{
  HAL_IWDG_Refresh(&hiwdg);
}

void board_fatal(void)
{
  __disable_irq();
  led_set(LED_FAULT, 1);
  for (;;) { }
}
