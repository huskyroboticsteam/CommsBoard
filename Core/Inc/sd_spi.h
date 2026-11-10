/**
  * sd_spi.h - microSD card over SPI1 (block driver used by FatFs).
  *
  * Follows ChaN's generic MMC/SD-over-SPI method. Supports SDHC/SDXC
  * (block addressing) and SDSC v1/v2 (byte addressing). Sectors are 512 bytes.
  */
#ifndef SD_SPI_H
#define SD_SPI_H

#include "main.h"
#include <stdint.h>

/* Chip select. If PA4 gets the CubeMX label "SD_CS", main.h defines these. */
#ifndef SD_CS_Pin
#define SD_CS_Pin       GPIO_PIN_4
#define SD_CS_GPIO_Port GPIOA
#endif

/* 1 = refuse to start when SD_DETECT (PC4) says no card (assumes low = inserted).
 * 0 = ignore the pin, for breadboard testing without a detect switch. */
#ifndef SD_USE_CARD_DETECT
#define SD_USE_CARD_DETECT 0
#endif

/* SPI1 runs from PCLK2 = 85 MHz (APB2 divider /2).
 * Slow: /256 = ~332 kHz, under the 400 kHz the SD spec asks for during start-up.
 * Fast: /8 = ~10.6 MHz for breadboard wires. Use /4 (~21 MHz) on the real PCB. */
#define SD_PRESCALER_SLOW SPI_BAUDRATEPRESCALER_256
#define SD_PRESCALER_FAST SPI_BAUDRATEPRESCALER_8

typedef enum {
  SD_OK = 0,
  SD_ERROR,     /* card answered, but with an error (or rejected the data) */
  SD_NO_CARD,   /* card detect says empty, or nothing answered on the bus */
  SD_TIMEOUT,   /* card stayed busy too long */
} SD_Result;

SD_Result SD_Init(void);
SD_Result SD_ReadBlocks(uint8_t *buf, uint32_t sector, uint32_t count);
SD_Result SD_WriteBlocks(const uint8_t *buf, uint32_t sector, uint32_t count);
SD_Result SD_Sync(void);              /* wait until the card finished writing */
uint32_t  SD_GetSectorCount(void);    /* 0 on error */
int       SD_IsCardInserted(void);

#endif /* SD_SPI_H */
