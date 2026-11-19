/* Minimal stand-in for the STM32 HAL so firmware files compile on a PC for tests/test_logger.c.
 * Only what sd_spi.c, the FatFs glue and the generated headers actually use. */
#ifndef FAKE_STM32G4XX_HAL_H
#define FAKE_STM32G4XX_HAL_H

#include <stdint.h>
#include <stddef.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;

typedef struct { uint32_t CR1; } SPI_TypeDef;
typedef struct { uint32_t BaudRatePrescaler; } SPI_InitTypeDef;
typedef struct { SPI_TypeDef *Instance; SPI_InitTypeDef Init; } SPI_HandleTypeDef;
#define SPI_CR1_BR_Pos 3
#define SPI_CR1_BR     (7u << SPI_CR1_BR_Pos)
#define MODIFY_REG(REG, CLEARMASK, SETMASK) ((REG) = (((REG) & ~(CLEARMASK)) | (SETMASK)))
#define __HAL_SPI_DISABLE(h) ((h)->Instance->CR1 &= ~(1u << 6))

typedef struct { int unused; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
extern GPIO_TypeDef fake_gpioa, fake_gpioc;
#define GPIOA (&fake_gpioa)
#define GPIOC (&fake_gpioc)
#define GPIO_PIN_0 0x0001u
#define GPIO_PIN_1 0x0002u
#define GPIO_PIN_2 0x0004u
#define GPIO_PIN_3 0x0008u
#define GPIO_PIN_4 0x0010u

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *tx, uint16_t n, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n);
HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *h, uint8_t *tx, uint16_t n);
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *h);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
uint32_t HAL_GetTick(void);
uint32_t HAL_RCC_GetPCLK2Freq(void);

/* Interrupt masking (board.h): nothing to mask on a PC. */
static inline uint32_t __get_PRIMASK(void) { return 0; }
static inline void __set_PRIMASK(uint32_t primask) { (void)primask; }
static inline void __disable_irq(void) { }

#endif
