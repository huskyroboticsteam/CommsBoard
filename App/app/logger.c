/**
  * logger.c - the only task that touches FatFs and the SD card.
  *
  * It takes binary records from the log ring, formats each as one JSON line, packs
  * whole lines into 512-byte blocks in a staging buffer, and writes the buffer when
  * it is full or once a second. While the card is missing, slow or broken this task
  * may block - CAN handling does not: the ring fills, and can_task drops and counts.
  *
  *   INIT ----> OPEN ----> LOGGING --(part full)--> OPEN
  *     |          |           |
  *     |          +--> CARD_FULL (no room for a new part)
  *     +-- fails 3 times --> FAULT --(every 10 s)--> INIT
  *
  * Chapter 6 adds NO_CARD (card detect), RECOVER (power-cycle a hung card) and
  * SHUTDOWN (power-fail).
  */
#include "app_main.h"
#include "board.h"
#include "config.h"
#include "jsonl.h"
#include "sd_spi.h"
#include "storage.h"
#include "timebase.h"
#include "timekeeping.h"
#include "cmsis_os.h"
#include <string.h>

#if __has_include("version.h")
#include "version.h"      /* written by tools/gen_version.sh before each build */
#endif
#ifndef FW_GIT_HASH
#define FW_GIT_HASH "unknown"
#endif

#define FLAG_RECORDS    0x1u    /* can_task: records are waiting */
#define FLAG_SD_DONE    0x2u    /* SD driver: a DMA transfer finished */
#define IDLE_WAIT_MS    100
#define RETRY_DELAY_MS  200     /* between card start-up tries */

extern osThreadId_t loggerTaskHandle;

static char stage_buf[JSON_STAGING_BYTES];
static jsonl_stage_t stage;
static logger_stats_t stats;
static uint32_t seq;            /* "n" of the next line; runs on across parts */
static uint64_t part_bytes;     /* written to the current part, header included */
static uint32_t last_flush_ms;
static uint32_t boot_rtc;       /* RTC time when this boot started logging */
static int boot_rtc_ok;
static int recovering;         /* the next part opens after a card fault */

/* ---- hooks the SD driver calls ---- */
static void sd_wait(uint32_t timeout_ms) { osThreadFlagsWait(FLAG_SD_DONE, osFlagsWaitAny, timeout_ms); }
static void sd_wake_isr(void)            { osThreadFlagsSet(loggerTaskHandle, FLAG_SD_DONE); }
static void sd_progress(void)            { wdg_checkin(WDG_LOGGER); }   /* the card answered: we are alive */

void logger_wake(void)
{
  osThreadFlagsSet(loggerTaskHandle, FLAG_RECORDS);
}

void logger_get_stats(logger_stats_t *out)
{
  *out = stats;
}

static void enter_fault(void)
{
  storage_close();
  stage.len = 0;                /* whatever was staged is lost with the card */
  stats.state = LOGGER_FAULT;
  stats.faults++;
  recovering = 1;
}

/* Write the staged blocks to the card. Returns 0 on success. */
static int flush(void)
{
  uint32_t t0 = osKernelGetTickCount();
  int res;

  last_flush_ms = t0;
  if (stage.len == 0) return 0;
  jsonl_stage_pad(&stage);
  led_set(LED_SD, 1);
  res = storage_write(stage.buf, (uint32_t)stage.len);
  led_set(LED_SD, 0);
  /* Chapter 6: a write slower than SD_WRITE_TIMEOUT_MS goes to RECOVER instead. */
  if (res != 0) return -1;
  stats.bytes += stage.len;
  part_bytes += stage.len;
  stage.len = 0;
  return 0;
}

/* Add one line to the stage, writing the stage out first if it is full. */
static int put_line(const char *line, size_t len)
{
  if (jsonl_stage_add(&stage, line, len)) return 0;
  if (flush() != 0) return -1;
  return jsonl_stage_add(&stage, line, len) ? 0 : -1;
}

/* A record the logger writes itself (not from the ring). */
static int put_sys(uint8_t event)
{
  char line[JSONL_MAX_LINE];
  uint64_t now = timebase_now_us();
  log_rec_t rec;

  logrec_sys(&rec, (uint32_t)now, event);
  stats.lines++;
  return put_line(line, jsonl_format(line, stats.boot_id, seq++, now, &rec));
}

/* INIT: start the card, mount it, work out the boot ID and check free space. */
static void do_init(void)
{
  static int tries;
  uint32_t free_mb, card_max, id;

  if (storage_mount(&free_mb, &card_max) != 0) {
    if (++tries >= SD_MAX_RECOVERIES) { tries = 0; enter_fault(); }
    else osDelay(RETRY_DELAY_MS);
    return;
  }
  tries = 0;

  if (stats.boot_id == 0) {
    /* Once per boot: 1 + the larger of the backup register and the highest ID on the
     * card, so it survives both a dead RTC battery and a swapped card. */
    id = timebase_boot_id_get();
    if (card_max > id) id = card_max;
    stats.boot_id = id + 1;
    timebase_boot_id_set(stats.boot_id);
    boot_rtc_ok = timebase_rtc_is_valid();
    boot_rtc = boot_rtc_ok ? tk_rtc_unix() : 0;
  }
  stats.state = (free_mb < LOG_MIN_FREE_MB) ? LOGGER_CARD_FULL : LOGGER_OPEN;
}

/* OPEN: create and preallocate the next part, with its header block. */
static void do_open(void)
{
  jsonl_hdr_t hdr = { FW_GIT_HASH, BOARD_NAME, stats.boot_id, board_reset_cause(),
                      boot_rtc, boot_rtc_ok, CAN_BITRATE_BPS };

  stage.len = jsonl_format_header(stage.buf, stage.cap, &hdr);
  jsonl_stage_pad(&stage);
  if (stats.part >= 99 || storage_open_part(stats.boot_id, stats.part + 1, LOG_PREALLOC_MB, stage.buf) != 0) {
    enter_fault();
    return;
  }
  stats.part++;
  stats.bytes += stage.len;
  part_bytes = stage.len;
  stage.len = 0;
  last_flush_ms = osKernelGetTickCount();
  stats.state = LOGGER_LOGGING;
  if (put_sys(recovering ? LOGREC_EV_RECOVER : LOGREC_EV_START) != 0) enter_fault();
  else recovering = 0;
}

/* LOGGING: drain the ring into JSON lines; write full stages, and at least once a second. */
static void do_logging(void)
{
  char line[JSONL_MAX_LINE];
  log_rec_t rec;
  uint64_t now = timebase_now_us();
  uint32_t n = 0;

  while (ringbuf_pop(&log_ring, &rec)) {
    if ((++n & 0xFF) == 0) now = timebase_now_us();     /* keep "now" fresh during long drains */
    stats.lines++;
    if (put_line(line, jsonl_format(line, stats.boot_id, seq++, tk_extend_us(now, rec.t_us), &rec)) != 0) {
      enter_fault();
      return;
    }
    /* This part is nearly full: finish it and start the next one. */
    if (part_bytes + stage.len + JSON_STAGING_BYTES > (uint64_t)LOG_PREALLOC_MB * 1024 * 1024) {
      if (flush() != 0) { enter_fault(); return; }
      storage_close();
      stats.state = LOGGER_OPEN;
      return;
    }
  }
  if (osKernelGetTickCount() - last_flush_ms >= LOG_FLUSH_INTERVAL_MS && flush() != 0) enter_fault();
}

void logger_task(void *argument)
{
  static const SD_Hooks hooks = { sd_wait, sd_wake_isr, sd_progress };
  uint32_t fault_ms = 0;

  (void)argument;
  SD_SetHooks(&hooks);
  jsonl_stage_init(&stage, stage_buf, sizeof stage_buf);
  stats.state = LOGGER_INIT;

  for (;;) {
    wdg_checkin(WDG_LOGGER);
    switch (stats.state) {
    case LOGGER_INIT:
      do_init();
      if (stats.state == LOGGER_FAULT) fault_ms = osKernelGetTickCount();
      break;
    case LOGGER_OPEN:
      do_open();
      if (stats.state == LOGGER_FAULT) fault_ms = osKernelGetTickCount();
      break;
    case LOGGER_LOGGING:
      osThreadFlagsWait(FLAG_RECORDS, osFlagsWaitAny, IDLE_WAIT_MS);
      do_logging();
      if (stats.state == LOGGER_FAULT) fault_ms = osKernelGetTickCount();
      break;
    case LOGGER_FAULT:
      /* Nothing is read from the ring, so it fills and can_task counts what is lost. */
      osDelay(IDLE_WAIT_MS);
      if (osKernelGetTickCount() - fault_ms >= SD_FAULT_RETRY_MS) stats.state = LOGGER_INIT;
      break;
    case LOGGER_CARD_FULL:
      osDelay(IDLE_WAIT_MS);    /* stays here: old files are never deleted automatically */
      break;
    }
  }
}
