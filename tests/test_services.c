/* Host unit tests for the hardware-free services: ringbuf, logrec, jsonl, timekeeping.
 * Build and run with `make -C tests`. Also writes sample log files that check_logs.py
 * parses with Python's json module and with tools/logread.py. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ringbuf.h"
#include "logrec.h"
#include "jsonl.h"
#include "timekeeping.h"

/* A fake RTC standing in for drivers/timebase.c. */
static rtc_civil_t fake_rtc;
static int fake_rtc_valid;
int timebase_rtc_is_valid(void) { return fake_rtc_valid; }
void timebase_rtc_read(rtc_civil_t *c) { *c = fake_rtc; }
void timebase_rtc_write(const rtc_civil_t *c) { fake_rtc = *c; fake_rtc_valid = 1; }

static void test_ringbuf(void)
{
  static log_rec_t store[8];
  ringbuf_t r;
  log_rec_t rec, out;
  uint32_t i, next_in = 0, next_out = 0;

  ringbuf_init(&r, store, 8);
  assert(ringbuf_count(&r) == 0 && !ringbuf_pop(&r, &out));

  for (i = 0; i < 7; i++) {                 /* 8 slots hold 7 records */
    logrec_drop(&rec, i, i);
    assert(ringbuf_push(&r, &rec));
  }
  assert(!ringbuf_push(&r, &rec));          /* full: refused, never waits */
  assert(ringbuf_count(&r) == 7 && r.high_water == 7);
  for (i = 0; i < 7; i++) {
    assert(ringbuf_pop(&r, &out) && out.t_us == i);   /* first in, first out */
  }
  assert(!ringbuf_pop(&r, &out));

  /* Many laps around the ring with uneven push/pop bursts. */
  for (i = 0; i < 10000; i++) {
    uint32_t burst = (i * 7) % 5 + 1, k;
    for (k = 0; k < burst; k++) {
      logrec_drop(&rec, next_in, next_in);
      if (ringbuf_push(&r, &rec)) next_in++;
    }
    burst = (i * 3) % 4 + 1;
    for (k = 0; k < burst; k++) {
      if (ringbuf_pop(&r, &out)) { assert(out.t_us == next_out); next_out++; }
    }
  }
  while (ringbuf_pop(&r, &out)) { assert(out.t_us == next_out); next_out++; }
  assert(next_in == next_out && next_in > 10000);
  puts("ringbuf      ok");
}

static void test_logrec(void)
{
  const uint8_t data[8] = { 0x02, 0x61, 0, 0, 0, 0, 0x01, 0xEE };
  log_rec_t r;

  assert(sizeof r == 16);
  logrec_frame(&r, LOGREC_RX, 5217, 0x408, 7, 0, data);
  assert(r.t_us == 5217 && r.id == 0x408 && r.dlc == 7 && r.type == LOGREC_RX);
  assert(memcmp(r.data, data, 7) == 0 && r.data[7] == 0);   /* bytes past dlc stay zero */

  logrec_frame(&r, LOGREC_TX, 1, 0xFFFF, 12, 1, data);       /* out-of-range inputs are clamped */
  assert(r.id == 0x7FF && r.dlc == 8 && r.type == (LOGREC_TX | LOGREC_RTR));

  logrec_time(&r, 9, 1762909680u);
  assert((r.type & LOGREC_KIND_MASK) == LOGREC_TIME && logrec_u32(&r, 0) == 1762909680u);
  logrec_tset(&r, 9, 111, 0xFFFFFFFFu);
  assert(logrec_u32(&r, 0) == 111 && logrec_u32(&r, 4) == 0xFFFFFFFFu);
  logrec_err(&r, 9, 255, 127, 5, LOGREC_ST_BUSOFF);
  assert(r.data[0] == 255 && r.data[1] == 127 && r.data[2] == 5 && r.data[3] == LOGREC_ST_BUSOFF);
  puts("logrec       ok");
}

static void expect_line(uint32_t b, uint32_t n, uint64_t t, const log_rec_t *r, const char *want)
{
  char line[JSONL_MAX_LINE];
  size_t len = jsonl_format(line, b, n, t, r);

  assert(len < JSONL_MAX_LINE && line[len - 1] == '\n');
  line[len - 1] = 0;
  if (strcmp(line, want) != 0) {
    printf("got : %s\nwant: %s\n", line, want);
    abort();
  }
}

static void test_jsonl_format(void)
{
  const uint8_t hb[7] = { 0x02, 0x61, 0, 0, 0, 0, 0x01 };
  const uint8_t one[1] = { 0x01 };
  const uint8_t full[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  char hdr[512];
  jsonl_hdr_t h = { "a1b2c3d", "NUCLEO", 42, "POR", 1762909680u, 1, 1000000 };
  log_rec_t r;
  size_t len;

  /* The example lines from the architecture document, byte for byte. */
  len = jsonl_format_header(hdr, sizeof hdr, &h);
  hdr[len - 1] = 0;
  assert(strcmp(hdr, "{\"k\":\"hdr\",\"v\":1,\"fw\":\"a1b2c3d\",\"board\":\"NUCLEO\",\"b\":42,"
                     "\"reset\":\"POR\",\"rtc\":1762909680,\"rtc_ok\":true,\"bitrate\":1000000}") == 0);
  logrec_time(&r, 0, 1762909680u);
  expect_line(42, 0, 5123, &r, "{\"b\":42,\"n\":0,\"t\":5123,\"k\":\"time\",\"rtc\":1762909680}");
  logrec_frame(&r, LOGREC_RX, 0, 0x408, 7, 0, hb);
  expect_line(42, 1, 5217, &r, "{\"b\":42,\"n\":1,\"t\":5217,\"k\":\"rx\",\"id\":\"0x408\",\"d\":\"02610000000001\"}");
  logrec_frame(&r, LOGREC_TX, 0, 0x10F, 1, 0, one);
  expect_line(42, 2, 5340, &r, "{\"b\":42,\"n\":2,\"t\":5340,\"k\":\"tx\",\"id\":\"0x10F\",\"d\":\"01\"}");

  /* The other record kinds. */
  logrec_frame(&r, LOGREC_RX, 0, 0x001, 4, 1, one);
  expect_line(1, 3, 7, &r, "{\"b\":1,\"n\":3,\"t\":7,\"k\":\"rx\",\"id\":\"0x001\",\"d\":\"\",\"r\":1}");
  logrec_err(&r, 0, 136, 0, 3, LOGREC_ST_PASSIVE);
  expect_line(1, 4, 8, &r, "{\"b\":1,\"n\":4,\"t\":8,\"k\":\"err\",\"tec\":136,\"rec\":0,\"lec\":3,\"st\":\"passive\"}");
  logrec_drop(&r, 0, 1234);
  expect_line(1, 5, 9, &r, "{\"b\":1,\"n\":5,\"t\":9,\"k\":\"drop\",\"cnt\":1234}");
  logrec_sys(&r, 0, LOGREC_EV_POWERFAIL);
  expect_line(1, 6, 10, &r, "{\"b\":1,\"n\":6,\"t\":10,\"k\":\"sys\",\"ev\":\"powerfail\"}");
  logrec_tset(&r, 0, 0, 1762909680u);
  expect_line(1, 7, 11, &r, "{\"b\":1,\"n\":7,\"t\":11,\"k\":\"tset\",\"old\":0,\"new\":1762909680}");

  /* 64-bit times around every digit-group boundary. */
  logrec_drop(&r, 0, 0);
  expect_line(0, 0, 4294967295ull, &r, "{\"b\":0,\"n\":0,\"t\":4294967295,\"k\":\"drop\",\"cnt\":0}");
  expect_line(0, 0, 4294967296ull, &r, "{\"b\":0,\"n\":0,\"t\":4294967296,\"k\":\"drop\",\"cnt\":0}");
  expect_line(0, 0, 5000000007ull, &r, "{\"b\":0,\"n\":0,\"t\":5000000007,\"k\":\"drop\",\"cnt\":0}");
  expect_line(0, 0, 18446744073709551615ull, &r,
              "{\"b\":0,\"n\":0,\"t\":18446744073709551615,\"k\":\"drop\",\"cnt\":0}");

  /* The longest possible line still fits. */
  logrec_frame(&r, LOGREC_RX, 0, 0x7FF, 8, 0, full);
  expect_line(4294967295u, 4294967295u, 18446744073709551615ull, &r,
              "{\"b\":4294967295,\"n\":4294967295,\"t\":18446744073709551615,\"k\":\"rx\","
              "\"id\":\"0x7FF\",\"d\":\"FFFFFFFFFFFFFFFF\"}");
  puts("jsonl format ok");
}

static void test_jsonl_stage(void)
{
  static char buf[4096];
  char line[JSONL_MAX_LINE];
  jsonl_stage_t s;
  size_t i, b;
  int added = 0;

  jsonl_stage_init(&s, buf, sizeof buf);
  memset(line, 'x', sizeof line);
  line[99] = '\n';
  while (jsonl_stage_add(&s, line, 100)) added++;      /* 5 lines of 100 fit per 512-byte block */
  assert(added == 40 && s.len <= sizeof buf);
  jsonl_stage_pad(&s);
  assert(s.len == 4096);

  for (b = 0; b < 8; b++) {                            /* every block ends on a line boundary */
    const char *blk = buf + b * 512;
    assert(blk[511] == '\n');
    for (i = 0; i < 500; i++) assert(blk[i] == (i % 100 == 99 ? '\n' : 'x'));
    for (i = 500; i < 511; i++) assert(blk[i] == ' ');
  }

  jsonl_stage_init(&s, buf, sizeof buf);               /* a line that exactly fills a block */
  memset(line, 'y', sizeof line);
  line[127] = '\n';
  for (i = 0; i < 4; i++) assert(jsonl_stage_add(&s, line, 128));
  assert(s.len == 512);
  jsonl_stage_pad(&s);
  assert(s.len == 512);                                /* nothing to pad */
  puts("jsonl stage  ok");
}

static void test_timekeeping(void)
{
  rtc_civil_t c = { 2000, 1, 1, 0, 0, 0 }, back;
  uint32_t t;

  assert(tk_civil_to_unix(&c) == 946684800u);
  c = (rtc_civil_t){ 2025, 11, 12, 1, 8, 0 };
  assert(tk_civil_to_unix(&c) == 1762909680u);         /* the time in the spec's example */
  c = (rtc_civil_t){ 2024, 2, 29, 23, 59, 59 };         /* leap day */
  assert(tk_civil_to_unix(&c) == 1709251199u);
  for (t = 946684800u; t < 4102444800u; t += 86399u * 13) {   /* 2000 to 2100, round trip */
    tk_unix_to_civil(t, &back);
    assert(tk_civil_to_unix(&back) == t);
    assert(back.month >= 1 && back.month <= 12 && back.day >= 1 && back.day <= 31);
  }

  /* Setting and reading the RTC goes through the same conversions. */
  assert(!timebase_rtc_is_valid());
  assert(tk_fattime() == (((2026u - 1980) << 25) | (1u << 21) | (1u << 16)));   /* placeholder date */
  tk_rtc_set_unix(1762909680u);
  assert(timebase_rtc_is_valid() && tk_rtc_unix() == 1762909680u);
  assert(fake_rtc.year == 2025 && fake_rtc.month == 11 && fake_rtc.day == 12 && fake_rtc.hour == 1);
  assert(tk_fattime() == (((2025u - 1980) << 25) | (11u << 21) | (12u << 16) | (1u << 11) | (8u << 5)));

  /* 32-bit record times are rebuilt correctly, including across the 71-minute wrap. */
  assert(tk_extend_us(5000, 4000) == 4000);
  assert(tk_extend_us(0x100000010ull, 0xFFFFFFF0u) == 0xFFFFFFF0ull);
  assert(tk_extend_us(0x300000005ull, 0x00000001u) == 0x300000001ull);
  assert(tk_extend_us(0x300000005ull, 0xFFFFFFFFu) == 0x2FFFFFFFFull);
  puts("timekeeping  ok");
}

/* Write a log file the way logger_task does: header block, then records packed into
 * 512-byte blocks. With torn = 1, the tail simulates a power cut: a half-written block
 * followed by leftover lines from an older boot in the preallocated space. */
static void write_sample(const char *path, uint32_t boot, uint32_t records, int torn)
{
  static char buf[4096];
  char line[JSONL_MAX_LINE];
  jsonl_hdr_t h = { "a1b2c3d", "NUCLEO", boot, "POR", 1762909680u, 1, 1000000 };
  jsonl_stage_t s;
  log_rec_t r;
  uint8_t data[8];
  uint32_t n;
  uint64_t t = 5000;
  size_t len;
  FILE *f = fopen(path, "wb");

  assert(f);
  jsonl_stage_init(&s, buf, sizeof buf);
  len = jsonl_format_header(buf, sizeof buf, &h);
  assert(len);
  s.len = len;
  jsonl_stage_pad(&s);                                 /* header fills the first block */

  for (n = 0; n < records; n++) {
    uint32_t i;
    t += 100 + n % 400;
    for (i = 0; i < 8; i++) data[i] = (uint8_t)(n * 31 + i * 7);
    switch (n % 97) {
    case 0:  logrec_time(&r, (uint32_t)t, 1762909680u + n / 97); break;
    case 50: logrec_drop(&r, (uint32_t)t, 3); break;
    case 60: logrec_err(&r, (uint32_t)t, 96, 0, 1, LOGREC_ST_WARN); break;
    case 70: logrec_sys(&r, (uint32_t)t, LOGREC_EV_START); break;
    default: logrec_frame(&r, n % 5 ? LOGREC_RX : LOGREC_TX, (uint32_t)t, (uint16_t)(n * 13),
                          (uint8_t)(n % 9), n % 41 == 0, data); break;
    }
    len = jsonl_format(line, boot, n, t, &r);
    if (!jsonl_stage_add(&s, line, len)) {             /* stage full: write it out, start again */
      jsonl_stage_pad(&s);
      fwrite(buf, 1, s.len, f);
      s.len = 0;
      assert(jsonl_stage_add(&s, line, len));
    }
  }
  jsonl_stage_pad(&s);
  fwrite(buf, 1, s.len, f);

  if (torn) {
    memset(buf, 0, 512);
    len = jsonl_format(line, boot, n, t + 1, &r);
    memcpy(buf, line, len / 2);                        /* half a line, then zeros */
    fwrite(buf, 1, 512, f);
    jsonl_stage_init(&s, buf, 512);                    /* a valid-looking block from an older boot */
    len = jsonl_format(line, boot - 1, n + 1, t + 2, &r);
    jsonl_stage_add(&s, line, len);
    jsonl_stage_pad(&s);
    fwrite(buf, 1, 512, f);
  }
  fclose(f);
}

int main(void)
{
  test_ringbuf();
  test_logrec();
  test_jsonl_format();
  test_jsonl_stage();
  test_timekeeping();
  write_sample("out/clean.jsonl", 42, 5000, 0);
  write_sample("out/torn.jsonl", 43, 3000, 1);
  puts("sample logs  written");
  return 0;
}
