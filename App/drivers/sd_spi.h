/**
  * sd_spi.h - microSD card over SPI1 (the block driver under FatFs).
  *
  * Follows ChaN's generic MMC/SD-over-SPI method. Supports SDHC/SDXC (block
  * addressing) and SDSC v1/v2 (byte addressing). Sectors are 512 bytes.
  * 512-byte data blocks move by DMA; every wait has a timeout.
  */
#ifndef SD_SPI_H
#define SD_SPI_H

#include <stdint.h>

typedef enum {
  SD_OK = 0,
  SD_ERROR,     /* card answered, but with an error (or rejected the data) */
  SD_NO_CARD,   /* nothing answered on the bus */
  SD_TIMEOUT,   /* card stayed busy too long */
} SD_Result;

typedef struct {
  uint32_t writes;        /* SD_WriteBlocks calls */
  uint32_t write_max_us;  /* slowest one */
  uint64_t write_sum_us;  /* total, for the average */
  uint32_t errors;        /* failed reads + writes */
  uint32_t polled_blocks; /* blocks sent without DMA because the buffer is out of DMA's reach (should be 0) */
} SD_Stats;

/* Optional hooks from the code that owns the card. Without them the driver just spins.
 *  wait(ms)   - called over and over while the driver waits on the card (start-up, a
 *               busy card, read data not ready, a DMA transfer in flight). Do other
 *               work here and return soon; ms is how much longer the driver is
 *               prepared to wait. It must not call back into this driver or FatFs.
 *  progress() - called whenever the card answers (lets a watchdog see we are alive) */
typedef struct {
  void (*wait)(uint32_t timeout_ms);
  void (*progress)(void);
} SD_Hooks;

void SD_SetHooks(const SD_Hooks *hooks);

SD_Result SD_Init(void);
SD_Result SD_ReadBlocks(uint8_t *buf, uint32_t sector, uint32_t count);
SD_Result SD_WriteBlocks(const uint8_t *buf, uint32_t sector, uint32_t count);
SD_Result SD_Sync(void);              /* wait until the card finished writing */
uint32_t  SD_GetSectorCount(void);    /* 0 on error */
const SD_Stats *SD_GetStats(void);

#endif /* SD_SPI_H */
