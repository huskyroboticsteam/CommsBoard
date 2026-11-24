/**
  * logger.c - the only code that touches FatFs and the SD card.
  *
  * Each call to logger_poll() does one step: it takes binary records from the log ring,
  * formats each as one JSON line, packs whole lines into 512-byte blocks in a staging
  * buffer, and writes the buffer when it is full or once a second.
  *
  * There are two staging buffers. While one is being written to the card, the SD driver
  * keeps calling our wait hook, and the hook formats the records that arrive meanwhile
  * into the other one. So formatting uses the time the card would otherwise make us wait.
  *
  * A card write can take a long time. While the SD driver waits on the card it calls
  * app_yield(), which keeps CAN routing running. If the card is missing, slow or broken,
  * the ring fills, and can_task drops and counts.
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
#include <string.h>

#if __has_include("version.h")
#include "version.h"      /* written by tools/gen_version.sh before each build */
#endif
#ifndef FW_GIT_HASH
#define FW_GIT_HASH "unknown"
#endif

#define RETRY_DELAY_MS  200     /* between card start-up tries */

/* Records carry only the low 32 bits of the microsecond clock, so a record's age can be
 * worked out only while it is under 2^31 us (35.8 minutes) old. While nothing is being
 * written (no card, card fault, card full), records older than this are thrown away and
 * counted, well before their timestamps would turn ambiguous. */
#define STALE_AGE_US    (30u * 60u * 1000000u)

static char stage_buf[2][JSON_STAGING_BYTES];
static jsonl_stage_t stages[2];
static jsonl_stage_t *fill = &stages[0];   /* the buffer lines are being added to */
static int writing;                        /* the other buffer is on its way to the card right now */
static logger_stats_t stats;
static uint32_t seq;            /* "n" of the next line; runs on across parts */
static uint64_t part_bytes;     /* written to the current part, header included */
static uint32_t last_flush_ms;
static uint32_t boot_rtc;       /* RTC time when this boot started logging */
static int boot_rtc_ok;
static int recovering;          /* the next part opens after a card fault */
static int init_tries;          /* failed card start-ups in a row */
static uint32_t wait_start_ms;  /* when the current retry delay or fault began */
static uint32_t stale_pending;  /* stale records thrown away and not yet reported in the log */

/* The SD driver calls this whenever the card answers: we are alive. */
static void sd_progress(void)
{
  wdg_checkin();
}

void logger_get_stats(logger_stats_t *out)
{
  *out = stats;
}

static void enter_fault(void)
{
  storage_close();
  stages[0].len = stages[1].len = 0;    /* whatever was staged is lost with the card */
  writing = 0;
  stats.state = LOGGER_FAULT;
  stats.faults++;
  recovering = 1;
  wait_start_ms = timebase_ms();
}

/* Turn waiting records into JSON lines in the fill buffer. Stops when the ring is empty
 * or the buffer is full (returns 1), leaving the record that did not fit in the ring.
 * Only touches the ring and RAM, never the card, so the SD wait hook may call it. */
static int format_records(void)
{
  char line[JSONL_MAX_LINE];
  const log_rec_t *rec;
  uint64_t now = timebase_now_us();
  uint32_t n = 0;
  size_t len;

  while ((rec = ringbuf_peek(&log_ring)) != 0) {
    if ((++n & 0xFF) == 0) now = timebase_now_us();     /* keep "now" fresh during long drains */
    len = jsonl_format(line, stats.boot_id, seq, tk_extend_us(now, rec->t_us), rec);
    if (!jsonl_stage_add(fill, line, len)) return 1;
    ringbuf_drop(&log_ring);
    seq++;
    stats.lines++;
    if (writing) stats.lines_during_write++;
  }
  return 0;
}

/* The SD driver calls this over and over while it waits on the card. */
static void sd_wait(void)
{
  app_yield();                      /* CAN routing and status must not wait for the card */
  if (writing) format_records();    /* fill the second buffer while the first is written */
}

/* Write the fill buffer to the card. Returns 0 on success.
 * With use_wait set, records that arrive during the write are formatted into the other
 * buffer, which then becomes the fill buffer. */
static int flush(int use_wait)
{
  jsonl_stage_t *out = fill;
  int res;

  last_flush_ms = timebase_ms();
  if (out->len == 0) return 0;
  jsonl_stage_pad(out);
  fill = (out == &stages[0]) ? &stages[1] : &stages[0];   /* empty: its own write finished earlier */
  writing = use_wait;
  led_set(LED_SD, 1);
  res = storage_write(out->buf, (uint32_t)out->len);
  led_set(LED_SD, 0);
  writing = 0;
  /* Chapter 6: a write slower than SD_WRITE_TIMEOUT_MS goes to RECOVER instead. */
  if (res != 0) return -1;
  stats.bytes += out->len;
  part_bytes += out->len;
  out->len = 0;
  return 0;
}

/* Add one of the logger's own lines. Only used right after a part is opened, when the
 * fill buffer is empty, so it always fits. */
static int put_line(const char *line, size_t len)
{
  return jsonl_stage_add(fill, line, len) ? 0 : -1;
}

/* A record the logger writes itself (not from the ring), stamped "now". */
static int put_own(log_rec_t *rec)
{
  char line[JSONL_MAX_LINE];
  uint64_t now = timebase_now_us();

  rec->t_us = (uint32_t)now;
  stats.lines++;
  return put_line(line, jsonl_format(line, stats.boot_id, seq++, now, rec));
}

static int put_sys(uint8_t event)
{
  log_rec_t rec;

  logrec_sys(&rec, 0, event);
  return put_own(&rec);
}

/* Throw away records too old to timestamp (see STALE_AGE_US). Only runs while the
 * logger is not draining the ring, so it never races the normal reader: it IS the reader. */
static void drop_stale(void)
{
  const log_rec_t *oldest;
  uint32_t now = timebase_now_lo();

  /* Signed age: a record stamped just after "now" was read is fresh, not ancient. */
  while ((oldest = ringbuf_peek(&log_ring)) != 0 && (int32_t)(now - oldest->t_us) > (int32_t)STALE_AGE_US) {
    ringbuf_drop(&log_ring);
    stale_pending++;
    stats.stale_dropped++;
  }
}

/* INIT: start the card, mount it, work out the boot ID and check free space. */
static void do_init(void)
{
  uint32_t free_mb, card_max, id;

  if (init_tries && timebase_ms() - wait_start_ms < RETRY_DELAY_MS) return;   /* between tries */
  if (storage_mount(&free_mb, &card_max) != 0) {
    wait_start_ms = timebase_ms();
    if (++init_tries >= SD_MAX_RECOVERIES) { init_tries = 0; enter_fault(); }
    return;
  }
  init_tries = 0;

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
                      boot_rtc, boot_rtc_ok, CAN_BITRATE_BPS, CAN_LOOPBACK };

  if (stats.part >= 99) {                   /* file names only have two digits for the part */
    stats.state = LOGGER_CARD_FULL;         /* stop here; retrying could never succeed */
    return;
  }
  /* Both buffers are empty here, so the fill buffer doubles as scratch for the header block. */
  fill->len = jsonl_format_header(fill->buf, fill->cap, &hdr);
  jsonl_stage_pad(fill);
  stats.part++;                             /* used up even if the open fails: a retry never reuses a name */
  if (storage_open_part(stats.boot_id, stats.part, LOG_PREALLOC_MB, fill->buf) != 0) {
    enter_fault();
    return;
  }
  stats.bytes += fill->len;
  part_bytes = fill->len;
  fill->len = 0;
  last_flush_ms = timebase_ms();
  stats.state = LOGGER_LOGGING;
  if (put_sys(recovering ? LOGREC_EV_RECOVER : LOGREC_EV_START) != 0) { enter_fault(); return; }
  recovering = 0;
  if (stale_pending) {          /* say how many records were thrown away while we were down */
    log_rec_t rec;

    logrec_drop(&rec, 0, stale_pending);
    if (put_own(&rec) != 0) { enter_fault(); return; }
    stale_pending = 0;
  }
}

/* LOGGING: drain the ring into JSON lines; write full buffers, and at least once a second. */
static void do_logging(void)
{
  const uint64_t part_size = (uint64_t)LOG_PREALLOC_MB * 1024 * 1024;
  int full;

  for (;;) {
    full = format_records();

    /* This part is nearly full: finish it and start the next one. Nothing is formatted
     * during this last write, so both buffers are empty when the next part opens. */
    if (part_bytes + fill->len + 2 * JSON_STAGING_BYTES > part_size) {
      if (flush(0) != 0) { enter_fault(); return; }
      storage_close();
      stats.state = LOGGER_OPEN;
      return;
    }
    if (!full) break;
    if (flush(1) != 0) { enter_fault(); return; }     /* the wait hook fills the other buffer meanwhile */
  }
  if (timebase_ms() - last_flush_ms >= LOG_FLUSH_INTERVAL_MS && flush(1) != 0) enter_fault();
}

void logger_init(void)
{
  static const SD_Hooks hooks = { sd_wait, sd_progress };

  SD_SetHooks(&hooks);
  jsonl_stage_init(&stages[0], stage_buf[0], sizeof stage_buf[0]);
  jsonl_stage_init(&stages[1], stage_buf[1], sizeof stage_buf[1]);
  stats.state = LOGGER_INIT;
}

void logger_poll(void)
{
  if (stats.state != LOGGER_LOGGING) drop_stale();

  switch (stats.state) {
  case LOGGER_INIT:
    do_init();
    break;
  case LOGGER_OPEN:
    do_open();
    break;
  case LOGGER_LOGGING:
    do_logging();
    break;
  case LOGGER_FAULT:
    /* Nothing is read from the ring, so it fills and can_task counts what is lost. */
    if (timebase_ms() - wait_start_ms >= SD_FAULT_RETRY_MS) stats.state = LOGGER_INIT;
    break;
  case LOGGER_CARD_FULL:
    break;                      /* stays here: old files are never deleted automatically */
  }
}
