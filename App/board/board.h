/**
  * board.h - the one file that knows which board this is: pins, peripherals and
  * interrupt priorities. Moving from the Nucleo to the real board changes this file
  * (and the CubeMX .ioc it mirrors) and nothing else.
  *
  * Pin labels (LED_HB_Pin, SD_CS_Pin, ...) come from CubeMX through main.h.
  */
#ifndef BOARD_H
#define BOARD_H

#define BOARD_NUCLEO            /* or BOARD_COMMS_V1 */

#include "main.h"

#if defined(BOARD_NUCLEO)
#define BOARD_NAME "NUCLEO"
#else
#define BOARD_NAME "COMMS_V1"
#endif

/* Peripheral handles (declared in the CubeMX-generated headers) */
#define BOARD_CAN_HANDLE      hfdcan2
#define BOARD_SD_SPI_HANDLE   hspi1
#define BOARD_DBG_UART_HANDLE huart2
#define BOARD_US_TIMER_HANDLE htim2    /* 32-bit, 1 MHz */

/* RAM that DMA cannot reach: the CCM SRAM, which the linker maps right after SRAM2. */
#define BOARD_CCM_START       0x20018000u

/* Interrupt priorities (architecture v2, section 3). Lower number = more urgent.
 * Anything that calls a FreeRTOS FromISR function must be 5 or higher. CubeMX forces
 * every one of these to 5, so board_init() sets the real values. */
#define BOARD_IRQ_PRIO_POWERFAIL  5
#define BOARD_IRQ_PRIO_CAN        6
#define BOARD_IRQ_PRIO_SD_DMA     7
#define BOARD_IRQ_PRIO_CARDDETECT 8
#define BOARD_IRQ_PRIO_US_TIMER   9
#define BOARD_IRQ_PRIO_DBG_UART   10

/* LEDs */
typedef enum { LED_HB = 0, LED_CAN, LED_SD, LED_FAULT, LED_COUNT } led_t;
void led_set(led_t led, int on);
void led_toggle(led_t led);

void board_init(void);                   /* priorities, debug freeze, reset cause */
const char *board_reset_cause(void);     /* "POR", "PIN", "IWDG", "SW", ... for the log header */
void board_watchdog_kick(void);
void board_fatal(void);                  /* fault LED on, stop; the watchdog then resets us */

#endif /* BOARD_H */
