/**
  * jsonl.h - turns binary log records into JSON Lines and packs them into 512-byte blocks.
  *
  * Numbers and hex are formatted by hand (no printf/snprintf), which is much faster
  * and uses almost no stack. No HAL here: this compiles and runs in the PC unit tests.
  */
#ifndef JSONL_H
#define JSONL_H

#include <stddef.h>
#include <stdint.h>
#include "logrec.h"

#define JSONL_VERSION     1
#define JSONL_MAX_LINE    128   /* longest possible record line, with the newline */
#define JSONL_BLOCK       512   /* SD sector size: lines never straddle a block */

/* Contents of the header line that starts every file. */
typedef struct {
  const char *fw;         /* firmware git hash */
  const char *board;      /* "NUCLEO" or "COMMS_V1" */
  uint32_t boot;          /* boot ID */
  const char *reset;      /* reset cause, e.g. "POR" */
  uint32_t rtc;           /* RTC Unix seconds at boot */
  int rtc_ok;             /* is the RTC time valid? */
  uint32_t bitrate;       /* CAN bit rate */
  int loopback;           /* CAN was in internal loopback: every tx frame also shows up as rx */
} jsonl_hdr_t;

/* Format one record as a JSON line ending in '\n'. out must hold JSONL_MAX_LINE bytes.
 * boot = boot ID, n = line sequence number, t_us = full 64-bit time since boot.
 * Returns the line length. */
size_t jsonl_format(char *out, uint32_t boot, uint32_t n, uint64_t t_us, const log_rec_t *r);

/* Format the header line. Returns its length, or 0 if it would not fit in cap. */
size_t jsonl_format_header(char *out, size_t cap, const jsonl_hdr_t *h);

/* ---- Block packing ----
 * A stage is a buffer that is a whole number of 512-byte blocks. Lines are added
 * whole: if the next line does not fit in the current block, the rest of that block
 * is filled with spaces and a final newline, and the line starts the next block. */
typedef struct {
  char *buf;
  size_t cap;   /* multiple of JSONL_BLOCK */
  size_t len;   /* bytes used */
} jsonl_stage_t;

void jsonl_stage_init(jsonl_stage_t *s, char *buf, size_t cap);
int jsonl_stage_add(jsonl_stage_t *s, const char *line, size_t n);  /* 0 = stage full, line not added */
void jsonl_stage_pad(jsonl_stage_t *s);    /* pad the last, partly filled block to 512 bytes */

#endif /* JSONL_H */
