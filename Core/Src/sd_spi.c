/**
  * sd_spi.c - microSD card over SPI1. See sd_spi.h.
  *
  * How SD-over-SPI works, in short:
  *  - Pull CS low, send a 6-byte command, then clock 0xFF until the card replies.
  *  - Data blocks start with a token byte (0xFE), then 512 bytes, then 2 CRC bytes.
  *  - While the card is busy writing it holds MISO low; 0xFF means "ready".
  */
#include "sd_spi.h"
#include "spi.h"
#include <string.h>

/* Commands. 0x80 marks an "application" command, which needs CMD55 sent first. */
#define CMD0    0       /* GO_IDLE_STATE: reset into SPI mode */
#define CMD8    8       /* SEND_IF_COND: tells v2 cards apart from v1 */
#define CMD9    9       /* SEND_CSD: card size info */
#define CMD12   12      /* STOP_TRANSMISSION */
#define CMD16   16      /* SET_BLOCKLEN */
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

#define SD_SPI_TIMEOUT_MS  100

static uint8_t card_type;   /* 0 = not initialized */

static void set_speed(uint32_t prescaler)
{
  __HAL_SPI_DISABLE(&hspi1);  /* the speed may only change while SPI is off; HAL turns it back on */
  MODIFY_REG(hspi1.Instance->CR1, SPI_CR1_BR, prescaler);
  hspi1.Init.BaudRatePrescaler = prescaler;
}

/* Send one byte and return the byte the card sent back at the same time. */
static uint8_t xchg(uint8_t out)
{
  uint8_t in = 0xFF;
  HAL_SPI_TransmitReceive(&hspi1, &out, &in, 1, SD_SPI_TIMEOUT_MS);
  return in;
}

/* Wait until the card stops holding MISO low (busy). 1 = ready, 0 = timed out. */
static int wait_ready(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  do {
    if (xchg(0xFF) == 0xFF) return 1;
  } while (HAL_GetTick() - start < timeout_ms);
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
  if (wait_ready(500)) return 1;
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
  return res;
}

/* Receive a data block of len bytes. 1 = OK. */
static int rcvr_datablock(uint8_t *buf, uint32_t len)
{
  uint8_t token;
  uint32_t start = HAL_GetTick();

  do {
    token = xchg(0xFF);
  } while (token == 0xFF && HAL_GetTick() - start < 200);
  if (token != 0xFE) return 0;

  /* Clock out 0xFF while reading. Same buffer for both is safe: each byte is
   * sent before its reply arrives. */
  memset(buf, 0xFF, len);
  if (HAL_SPI_TransmitReceive(&hspi1, buf, buf, (uint16_t)len, SD_SPI_TIMEOUT_MS) != HAL_OK) return 0;
  xchg(0xFF);                   /* discard CRC */
  xchg(0xFF);
  return 1;
}

/* Send one 512-byte block (or just the stop token 0xFD when buf is NULL). 1 = OK. */
static int xmit_datablock(const uint8_t *buf, uint8_t token)
{
  uint8_t resp;

  if (!wait_ready(500)) return 0;
  xchg(token);
  if (token == 0xFD) return 1;

  if (HAL_SPI_Transmit(&hspi1, (uint8_t *)buf, 512, SD_SPI_TIMEOUT_MS) != HAL_OK) return 0;
  xchg(0xFF);                   /* dummy CRC */
  xchg(0xFF);
  resp = xchg(0xFF);            /* data response: xxx00101 = accepted */
  return (resp & 0x1F) == 0x05;
}

int SD_IsCardInserted(void)
{
#if SD_USE_CARD_DETECT
  return HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) == GPIO_PIN_RESET;
#else
  return 1;
#endif
}

SD_Result SD_Init(void)
{
  uint8_t ty = 0, ocr[4];
  uint32_t start;
  int i;

  card_type = 0;
  if (!SD_IsCardInserted()) return SD_NO_CARD;

  set_speed(SD_PRESCALER_SLOW);
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
  for (i = 0; i < 10; i++) xchg(0xFF);  /* 80+ clocks with CS high: wake the card */

  if (send_cmd(CMD0, 0) == 1) {         /* card is now in idle state */
    start = HAL_GetTick();
    if (send_cmd(CMD8, 0x1AA) == 1) {   /* SD v2 */
      for (i = 0; i < 4; i++) ocr[i] = xchg(0xFF);
      if (ocr[2] == 0x01 && ocr[3] == 0xAA) {      /* card works at 3.3 V */
        while (HAL_GetTick() - start < 1000 && send_cmd(ACMD41, 1UL << 30)) ;
        if (HAL_GetTick() - start < 1000 && send_cmd(CMD58, 0) == 0) {
          for (i = 0; i < 4; i++) ocr[i] = xchg(0xFF);
          ty = (ocr[0] & 0x40) ? (CT_SDC2 | CT_BLOCK) : CT_SDC2;
        }
      }
    } else if (send_cmd(ACMD41, 0) <= 1) {          /* SD v1 */
      ty = CT_SDC1;
      while (HAL_GetTick() - start < 1000 && send_cmd(ACMD41, 0)) ;
      if (HAL_GetTick() - start >= 1000 || send_cmd(CMD16, 512) != 0) ty = 0;
    }
    /* Old MMC cards are not supported. */
  }
  cs_deselect();

  if (!ty) return SD_NO_CARD;
  card_type = ty;
  set_speed(SD_PRESCALER_FAST);
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
  return res;
}

SD_Result SD_WriteBlocks(const uint8_t *buf, uint32_t sector, uint32_t count)
{
  SD_Result res = SD_ERROR;

  if (!card_type || count == 0) return SD_ERROR;
  if (!(card_type & CT_BLOCK)) sector *= 512;

  if (count == 1) {
    if (send_cmd(CMD24, sector) == 0 && xmit_datablock(buf, 0xFE)) res = SD_OK;
  } else if (send_cmd(CMD25, sector) == 0) {
    do {
      if (!xmit_datablock(buf, 0xFC)) break;
      buf += 512;
    } while (--count);
    if (!xmit_datablock(NULL, 0xFD)) count = 1;     /* stop token */
    if (count == 0) res = SD_OK;
  }
  cs_deselect();
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
