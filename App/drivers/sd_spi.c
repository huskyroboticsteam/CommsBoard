/**
  * sd_spi.c - microSD card over SPI1. See sd_spi.h.
  *
  * How SD-over-SPI works, in short:
  *  - Pull CS low, send a 6-byte command, then clock 0xFF until the card replies.
  *  - Data blocks start with a token byte (0xFE), then 512 bytes, then 2 CRC bytes.
  *  - While the card is busy writing it holds MISO low; 0xFF means "ready".
  */
#include "sd_spi.h"
#include "board.h"
#include "config.h"
#include "spi.h"
#include "timebase.h"
#include <string.h>

#define hspi BOARD_SD_SPI_HANDLE

/* Commands. 0x80 marks an "application" command, which needs CMD55 sent first. */
#define CMD0    0       /* GO_IDLE_STATE: reset into SPI mode */
#define CMD8    8       /* SEND_IF_COND: tells v2 cards apart from v1 */
#define CMD9    9       /* SEND_CSD: card size info */
#define CMD12   12      /* STOP_TRANSMISSION */
#define CMD16   16      /* SET_BLOCKLEN */
#define ACMD23  (0x80 | 23) /* SET_WR_BLK_ERASE_COUNT: pre-erase before a multi-block write */
#define CMD17   17      /* READ_SINGLE_BLOCK */
#define CMD18   18      /* READ_MULTIPLE_BLOCK */
#define CMD24   24      /* WRITE_BLOCK */
#define CMD25   25      /* WRITE_MULTIPLE_BLOCK */
#define CMD55   55      /* APP_CMD */
#define CMD58   58      /* READ_OCR: tells SDHC apart from SDSC */
#define ACMD41  (0x80 | 41) /* SD_SEND_OP_COND: start initialization */

/* Card type bits */
#define CT_SDC1   0x01  /* SD v1 */
#define CT_SDC2   0x02  /* SD v2 */
#define CT_BLOCK  0x04  /* block addressing (SDHC/SDXC) */

#define SPI_TIMEOUT_MS    100   /* one SPI transfer */
#define READ_TOKEN_MS     200   /* card must start sending read data within this */
#define SPIN_BEFORE_YIELD_US 2000  /* poll flat out this long, then let other work run between polls */

enum { DMA_IDLE = 0, DMA_BUSY, DMA_DONE, DMA_ERROR };

static uint8_t card_type;   /* 0 = not initialized */
static SD_Hooks hooks;
static SD_Stats stats;
static volatile uint8_t dma_state;

void SD_SetHooks(const SD_Hooks *h)
{
  hooks = *h;
}

const SD_Stats *SD_GetStats(void)
{
  return &stats;
}

static void spi_byte_mode(void);

/* Pick the fastest SPI clock that does not exceed hz (SPI1 runs from PCLK2). */
static void set_speed(uint32_t hz)
{
  uint32_t pclk = HAL_RCC_GetPCLK2Freq();
  uint32_t br = 0;                      /* divider = 2 << br */

  while (br < 7 && (pclk >> (br + 1)) > hz) br++;
  __HAL_SPI_DISABLE(&hspi);             /* the speed may only change while SPI is off; HAL turns it back on */
  MODIFY_REG(hspi.Instance->CR1, SPI_CR1_BR, br << SPI_CR1_BR_Pos);
  hspi.Init.BaudRatePrescaler = br << SPI_CR1_BR_Pos;
  spi_byte_mode();
}

/* Let the rest of the firmware run for a moment. Every loop that waits on the card
 * calls this, so a slow or absent card never holds up CAN handling. */
static void yield(void)
{
  if (hooks.wait) hooks.wait();
}

#ifdef SD_SPI_HAL_XCHG
/* PC tests: the fake HAL has no registers; the SD card emulator sits behind
 * HAL_SPI_TransmitReceive(). */
static void spi_byte_mode(void) { }

static uint8_t xchg(uint8_t out)
{
  uint8_t in = 0xFF;
  HAL_SPI_TransmitReceive(&hspi, &out, &in, 1, SPI_TIMEOUT_MS);
  return in;
}
#else
/* Single bytes go straight through the SPI registers. Every command byte, every
 * "are you busy?" poll and every CRC byte is a 1-byte transfer, and HAL_SPI_TransmitReceive()
 * spends far longer checking state and setting up than the byte itself takes on the wire
 * (0.4 us at 21 MHz). Block transfers still use the HAL (DMA or polled).
 *
 * spi_byte_mode() must run after any HAL transfer and after a speed change: the HAL
 * leaves the RX FIFO threshold at 16 bits after multi-byte transfers (RXNE would then
 * never fire for one byte) and set_speed() turns the SPI off. It also empties the RX FIFO,
 * so the next byte read really is the reply to the next byte sent. */
static void spi_byte_mode(void)
{
  SPI_TypeDef *spi = hspi.Instance;

  SET_BIT(spi->CR2, SPI_CR2_FRXTH);              /* RXNE as soon as one byte is in */
  while (spi->SR & SPI_SR_RXNE) (void)*(volatile uint8_t *)&spi->DR;
  (void)spi->SR;                                 /* with the DR read above, clears a stale overrun */
  SET_BIT(spi->CR1, SPI_CR1_SPE);
}

static uint8_t xchg(uint8_t out)
{
  SPI_TypeDef *spi = hspi.Instance;

  while (!(spi->SR & SPI_SR_TXE)) { }
  *(volatile uint8_t *)&spi->DR = out;           /* 8-bit access: a 16-bit write would send two bytes */
  while (!(spi->SR & SPI_SR_RXNE)) { }           /* master, full duplex: always arrives after 8 clocks */
  return *(volatile uint8_t *)&spi->DR;
}
#endif

/* Wait until the card stops holding MISO low (busy). 1 = ready, 0 = timed out.
 * Polls flat out at first (the usual wait is short), then calls the wait hook between
 * polls so a long card stall does not hold up the rest of the firmware. */
static int wait_ready(uint32_t timeout_ms)
{
  uint32_t start_ms = HAL_GetTick();
  uint32_t start_us = timebase_now_lo();

  do {
    if (xchg(0xFF) == 0xFF) return 1;
    if (timebase_now_lo() - start_us > SPIN_BEFORE_YIELD_US) yield();
  } while (HAL_GetTick() - start_ms < timeout_ms);
  return 0;
}

static void cs_deselect(void)
{
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
  xchg(0xFF);   /* one extra byte of clock so the card lets go of MISO */
}

static int cs_select(void)
{
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET);
  xchg(0xFF);
  if (wait_ready(SD_WRITE_TIMEOUT_MS)) return 1;
  cs_deselect();
  return 0;
}

/* Send a command and return the card's 1-byte reply (R1). 0xFF = no reply. */
static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
  uint8_t res, n;

  if (cmd & 0x80) {             /* application command: send CMD55 first */
    cmd &= 0x7F;
    res = send_cmd(CMD55, 0);
    if (res > 1) return res;
  }

  if (cmd != CMD12) {
    cs_deselect();
    if (!cs_select()) return 0xFF;
  }

  xchg(0x40 | cmd);
  xchg((uint8_t)(arg >> 24));
  xchg((uint8_t)(arg >> 16));
  xchg((uint8_t)(arg >> 8));
  xchg((uint8_t)arg);
  /* The CRC is only checked for CMD0 and CMD8 in SPI mode. */
  xchg(cmd == CMD0 ? 0x95 : cmd == CMD8 ? 0x87 : 0x01);

  if (cmd == CMD12) xchg(0xFF); /* skip the stuff byte after a stop */

  n = 10;                       /* the reply comes within 8 bytes */
  do {
    res = xchg(0xFF);
  } while ((res & 0x80) && --n);
  if (res != 0xFF && hooks.progress) hooks.progress();
  return res;
}

/* ---- 512-byte data transfers: DMA when possible, polling otherwise ---- */

/* Should this block go by DMA? Without a wait hook there is nothing useful to do while
 * a DMA transfer runs, so plain polling is just as good. DMA also cannot reach the CCM
 * SRAM: a buffer there still works, by polling, but slower and without yielding, so it
 * is counted and shows up in the stats line. */
static int can_dma(const void *buf, uint32_t len)
{
  uintptr_t a = (uintptr_t)buf;

  if (len != 512 || !hooks.wait) return 0;
  if (a >= 0x20000000u && a + len <= BOARD_CCM_START) return 1;
  stats.polled_blocks++;
  return 0;
}

static int dma_wait(void)
{
  uint32_t start = HAL_GetTick();

  while (dma_state == DMA_BUSY) {
    if (HAL_GetTick() - start > SPI_TIMEOUT_MS) {
      HAL_SPI_Abort(&hspi);
      return 0;
    }
    yield();                      /* other work runs while the DMA moves the block */
  }
  return dma_state == DMA_DONE;
}

static void dma_finished(SPI_HandleTypeDef *h, uint8_t state)
{
  if (h != &hspi) return;
  dma_state = state;
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *h) { dma_finished(h, DMA_DONE); }
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *h)   { dma_finished(h, DMA_DONE); }
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *h)    { dma_finished(h, DMA_ERROR); }

/* Receive a data block of len bytes. 1 = OK. */
static int rcvr_datablock(uint8_t *buf, uint32_t len)
{
  uint8_t token;
  uint32_t start = HAL_GetTick();
  uint32_t start_us = timebase_now_lo();

  for (;;) {                    /* the card sends 0xFF until the data is ready */
    token = xchg(0xFF);
    if (token != 0xFF || HAL_GetTick() - start >= READ_TOKEN_MS) break;
    if (timebase_now_lo() - start_us > SPIN_BEFORE_YIELD_US) yield();
  }
  if (token != 0xFE) return 0;

  /* Clock out 0xFF while reading. Same buffer for both is safe: each byte is
   * sent before its reply arrives. */
  memset(buf, 0xFF, len);
  if (can_dma(buf, len)) {
    dma_state = DMA_BUSY;
    if (HAL_SPI_TransmitReceive_DMA(&hspi, buf, buf, (uint16_t)len) != HAL_OK || !dma_wait()) {
      spi_byte_mode();
      return 0;
    }
  } else if (HAL_SPI_TransmitReceive(&hspi, buf, buf, (uint16_t)len, SPI_TIMEOUT_MS) != HAL_OK) {
    spi_byte_mode();
    return 0;
  }
  spi_byte_mode();
  xchg(0xFF);                   /* discard CRC */
  xchg(0xFF);
  if (hooks.progress) hooks.progress();
  return 1;
}

/* Send one 512-byte block (or just the stop token 0xFD when buf is NULL). 1 = OK. */
static int xmit_datablock(const uint8_t *buf, uint8_t token)
{
  uint8_t resp;

  if (!wait_ready(SD_WRITE_TIMEOUT_MS)) return 0;
  xchg(token);
  if (token == 0xFD) return 1;

  if (can_dma(buf, 512)) {
    dma_state = DMA_BUSY;
    if (HAL_SPI_Transmit_DMA(&hspi, (uint8_t *)buf, 512) != HAL_OK || !dma_wait()) {
      spi_byte_mode();
      return 0;
    }
  } else if (HAL_SPI_Transmit(&hspi, (uint8_t *)buf, 512, SPI_TIMEOUT_MS) != HAL_OK) {
    spi_byte_mode();
    return 0;
  }
  spi_byte_mode();              /* transmit-only: drops the bytes the card clocked back */
  xchg(0xFF);                   /* dummy CRC */
  xchg(0xFF);
  resp = xchg(0xFF);            /* data response: xxx00101 = accepted */
  if ((resp & 0x1F) != 0x05) return 0;
  if (hooks.progress) hooks.progress();
  return 1;
}

SD_Result SD_Init(void)
{
  uint8_t ty = 0, ocr[4];
  uint32_t start;
  int i;

  card_type = 0;
  set_speed(SD_SPI_INIT_HZ);
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
  for (i = 0; i < 10; i++) xchg(0xFF);  /* 80+ clocks with CS high: wake the card */

  if (send_cmd(CMD0, 0) == 1) {         /* card is now in idle state */
    start = HAL_GetTick();
    if (send_cmd(CMD8, 0x1AA) == 1) {   /* SD v2 */
      for (i = 0; i < 4; i++) ocr[i] = xchg(0xFF);
      if (ocr[2] == 0x01 && ocr[3] == 0xAA) {      /* card works at 3.3 V */
        while (HAL_GetTick() - start < 1000 && send_cmd(ACMD41, 1UL << 30)) yield();   /* can take a second */
        if (HAL_GetTick() - start < 1000 && send_cmd(CMD58, 0) == 0) {
          for (i = 0; i < 4; i++) ocr[i] = xchg(0xFF);
          ty = (ocr[0] & 0x40) ? (CT_SDC2 | CT_BLOCK) : CT_SDC2;
        }
      }
    } else if (send_cmd(ACMD41, 0) <= 1) {          /* SD v1 */
      ty = CT_SDC1;
      while (HAL_GetTick() - start < 1000 && send_cmd(ACMD41, 0)) yield();
      if (HAL_GetTick() - start >= 1000) ty = 0;
    }
    /* Old MMC cards are not supported. */
    if (ty && !(ty & CT_BLOCK) && send_cmd(CMD16, 512) != 0) ty = 0;   /* byte-addressed: 512-byte blocks */
  }
  cs_deselect();

  if (!ty) return SD_NO_CARD;
  card_type = ty;
  set_speed(SD_SPI_FAST_HZ);
  return SD_OK;
}

SD_Result SD_ReadBlocks(uint8_t *buf, uint32_t sector, uint32_t count)
{
  SD_Result res = SD_ERROR;

  if (!card_type || count == 0) return SD_ERROR;
  if (!(card_type & CT_BLOCK)) sector *= 512;       /* SDSC uses byte addresses */

  if (count == 1) {
    if (send_cmd(CMD17, sector) == 0 && rcvr_datablock(buf, 512)) res = SD_OK;
  } else if (send_cmd(CMD18, sector) == 0) {
    do {
      if (!rcvr_datablock(buf, 512)) break;
      buf += 512;
    } while (--count);
    send_cmd(CMD12, 0);
    if (count == 0) res = SD_OK;
  }
  cs_deselect();
  if (res != SD_OK) stats.errors++;
  return res;
}

SD_Result SD_WriteBlocks(const uint8_t *buf, uint32_t sector, uint32_t count)
{
  SD_Result res = SD_ERROR;
  uint32_t t0 = timebase_now_lo(), dt;

  if (!card_type || count == 0) return SD_ERROR;
  if (!(card_type & CT_BLOCK)) sector *= 512;

  if (count == 1) {
    if (send_cmd(CMD24, sector) == 0 && xmit_datablock(buf, 0xFE)) res = SD_OK;
  } else {
    /* Tell the card how many blocks are coming so it can erase them up front: many
     * cards write a multi-block run noticeably faster this way. Every card this driver
     * accepts is an SD card (MMC is refused in SD_Init), and the reply does not matter. */
    send_cmd(ACMD23, count);
    if (send_cmd(CMD25, sector) == 0) {
      do {
        if (!xmit_datablock(buf, 0xFC)) break;
        buf += 512;
      } while (--count);
      if (!xmit_datablock(NULL, 0xFD)) count = 1;   /* stop token */
      if (count == 0) res = SD_OK;
    }
  }
  /* The card is busy until the data is really stored: wait here so the time we
   * measure, and the result we return, cover the whole write. */
  if (res == SD_OK && !wait_ready(SD_WRITE_TIMEOUT_MS)) res = SD_TIMEOUT;
  cs_deselect();

  dt = timebase_now_lo() - t0;
  stats.writes++;
  stats.write_sum_us += dt;
  if (dt > stats.write_max_us) stats.write_max_us = dt;
  if (res != SD_OK) stats.errors++;
  return res;
}

SD_Result SD_Sync(void)
{
  SD_Result res = cs_select() ? SD_OK : SD_TIMEOUT; /* cs_select() waits for not-busy */
  cs_deselect();
  return res;
}

uint32_t SD_GetSectorCount(void)
{
  uint8_t csd[16];
  uint32_t csize, sectors = 0;
  uint8_t n;

  if (!card_type) return 0;
  if (send_cmd(CMD9, 0) == 0 && rcvr_datablock(csd, 16)) {
    if ((csd[0] >> 6) == 1) {           /* CSD v2 (SDHC/SDXC) */
      csize = csd[9] + ((uint32_t)csd[8] << 8) + ((uint32_t)(csd[7] & 0x3F) << 16) + 1;
      sectors = csize << 10;
    } else {                            /* CSD v1 (SDSC) */
      n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
      csize = (csd[8] >> 6) + ((uint32_t)csd[7] << 2) + ((uint32_t)(csd[6] & 3) << 10) + 1;
      sectors = csize << (n - 9);
    }
  }
  cs_deselect();
  return sectors;
}
