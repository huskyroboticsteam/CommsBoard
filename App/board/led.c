/* led.c - the four status LEDs. Pins come from the CubeMX labels in main.h. */
#include "board.h"

static GPIO_TypeDef *const LED_PORT[LED_COUNT] = {
  LED_HB_GPIO_Port, LED_CAN_GPIO_Port, LED_SD_GPIO_Port, LED_FAULT_GPIO_Port
};
static const uint16_t LED_PIN[LED_COUNT] = {
  LED_HB_Pin, LED_CAN_Pin, LED_SD_Pin, LED_FAULT_Pin
};

void led_set(led_t led, int on)
{
  HAL_GPIO_WritePin(LED_PORT[led], LED_PIN[led], on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void led_toggle(led_t led)
{
  HAL_GPIO_TogglePin(LED_PORT[led], LED_PIN[led]);
}
