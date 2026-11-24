/* Host integration test: the real logger.c, storage.c, sd_spi.c, FatFs and its glue,
 * running on a PC against a byte-level SD card emulator.
 *
 * Each "boot" runs the main loop's logger_poll() in a forked child (fresh static state,
 * like a reset) while the test plays the part of can_task and feeds records into the
 * log ring. The
 * card image is shared memory, so it survives from one boot to the next. Afterwards the
 * log files are copied off the card into out/card/ for check_logs.py and logread.py.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "app_main.h"
#include "board.h"
#include "config.h"
#include "app_fatfs.h"
#include "sd_spi.h"
#include "spi.h"
#include "storage.h"
#include "timebase.h"

#define CARD_SECTORS (160u * 2048)     /* 160 MB: room for 6 parts of 16 MB, then "card full" */

/* ---------------- fake hardware ---------------- */
GPIO_TypeDef fake_gpioa, fake_gpioc;
static SPI_TypeDef spi_regs;
SPI_HandleTypeDef hspi1 = { &spi_regs, { 0 } };

static uint32_t tick_ms;
static uint64_t clock_us;
uint32_t HAL_GetTick(void) { return tick_ms++; }
uint32_t timebase_ms(void) { return tick_ms; }
uint32_t HAL_RCC_GetPCLK2Freq(void) { return 85000000; }
uint64_t timebase_now_us(void)
{
  clock_us += 7;
  if (clock_us < (uint64_t)tick_ms * 1000) clock_us = (uint64_t)tick_ms * 1000;
  return clock_us;
}
uint32_t timebase_now_lo(void) { return (uint32_t)timebase_now_us(); }
int timebase_rtc_is_valid(void) { return 0; }
void timebase_rtc_read(rtc_civil_t *c) { memset(c, 0, sizeof *c); }
void timebase_rtc_write(const rtc_civil_t *c) { (void)c; }
static uint32_t boot_id_reg;           /* the backup register: lost on every fork, like a dead battery */
uint32_t timebase_boot_id_get(void) { return boot_id_reg; }
void timebase_boot_id_set(uint32_t id) { boot_id_reg = id; }
void led_set(led_t led, int on) { (void)led; (void)on; }
const char *board_reset_cause(void) { return "POR"; }
void wdg_checkin(void) { }
void app_yield(void) { }                /* the SD driver's "do other work while waiting" hook */

static log_rec_t log_store[LOG_RING_RECORDS];
ringbuf_t log_ring;

/* ---------------- SD card emulator (SDHC, SPI mode) ---------------- */
static uint8_t *img;                   /* CARD_SECTORS * 512 bytes of shared memory */
static int cs_low, reject_writes;
static int read_delay;                 /* 0xFF bytes the card sends before read data (a slow card) */
static int busy_len = 3;               /* bytes the card stays busy after each block written */
static uint8_t q[2048]; static unsigned qh, qt;      /* bytes the card will send */
static uint8_t cmd[6]; static int cmdlen;
static int idle, app, acmd41_calls;
static int acmd23_calls, acmd23_bad;   /* pre-erase counts announced, and ones the next write did not match */
static uint32_t acmd23_count;          /* blocks the last ACMD23 announced (0 = none pending) */
static int wr_mode;                    /* 0 none, 1 single, 2 multi */
static int wr_cnt; static uint32_t wr_addr; static uint8_t wr_buf[514];
static int rd_multi; static uint32_t rd_addr;

static void put(uint8_t b) { q[qt++ % sizeof q] = b; }
static void put_block(uint32_t s)
{
  assert(s < CARD_SECTORS);
  put(0xFE);
  for (int i = 0; i < 512; i++) put(img[(size_t)s * 512 + i]);
  put(0); put(0);
}

static void do_cmd(void)
{
  uint8_t c = cmd[0] & 0x3F;
  uint32_t arg = (uint32_t)cmd[1] << 24 | (uint32_t)cmd[2] << 16 | (uint32_t)cmd[3] << 8 | cmd[4];
  int was_app = app;

  app = 0;
  put(0xFF);                                          /* one byte of delay before the reply */
  if (c == 0)  { assert(cmd[5] == 0x95); idle = 1; acmd41_calls = 0; put(0x01); return; }
  if (c == 8)  { assert(cmd[5] == 0x87); put(0x01); put(0); put(0); put(cmd[3]); put(cmd[4]); return; }
  if (c == 55) { app = 1; put((uint8_t)idle); return; }
  if (c == 23 && was_app && !idle) { acmd23_calls++; acmd23_count = arg & 0x7FFFFF; put(0); return; }
  if (c == 41 && was_app) { if (++acmd41_calls >= 3) idle = 0; put((uint8_t)idle); return; }
  if (idle)    { put(0x05); return; }                 /* not initialised yet */
  if (c == 58) { put(0); put(0xC0); put(0xFF); put(0x80); put(0); return; }   /* OCR: ready, SDHC */
  if (c == 9) {
    uint32_t cs = CARD_SECTORS / 1024 - 1;
    uint8_t csd[16] = { 0x40, 0, 0, 0, 0, 0, 0, (uint8_t)(cs >> 16 & 0x3F), (uint8_t)(cs >> 8), (uint8_t)cs };
    put(0); put(0xFE);
    for (int i = 0; i < 16; i++) put(csd[i]);
    put(0); put(0);
    return;
  }
  if (c == 17 || c == 18) {
    put(0);
    for (int i = 0; i < read_delay; i++) put(0xFF);
    put_block(arg);
    if (c == 18) { rd_multi = 1; rd_addr = arg + 1; }
    return;
  }
  if (c == 12) { rd_multi = 0; qh = qt; put(0xFF); put(0); put(0); put(0); return; }   /* stuff, R1, busy */
  if (c == 25 && acmd23_count == 0) acmd23_bad++;      /* every multi-block write is announced */
  if (c == 24 || c == 25) acmd23_count = 0;            /* an announcement covers the next write only */
  if (c == 24 || c == 25) { put(0); wr_mode = c == 24 ? 1 : 2; wr_addr = arg; wr_cnt = -1; return; }
  put(0x04);                                          /* illegal command */
}

static uint8_t card_byte(uint8_t in)
{
  uint8_t out;

  if (!cs_low) return 0xFF;
  out = qh != qt ? q[qh++ % sizeof q] : 0xFF;
  if (qh == qt && rd_multi) put_block(rd_addr++);
  if (wr_mode) {
    if (wr_cnt < 0) {                                 /* waiting for a data token */
      if (in == 0xFE || in == 0xFC) wr_cnt = 0;
      else if (in == 0xFD && wr_mode == 2) { wr_mode = 0; put(0xFF); put(0); put(0); }
      return out;
    }
    wr_buf[wr_cnt++] = in;
    if (wr_cnt == 514) {
      if (reject_writes) put(0x0D);                   /* data rejected: write error */
      else { assert(wr_addr < CARD_SECTORS); memcpy(img + (size_t)wr_addr++ * 512, wr_buf, 512); put(0x05); }
      for (int i = 0; i < busy_len; i++) put(0);      /* busy for a while */
      wr_cnt = -1;
      if (wr_mode == 1) wr_mode = 0;
    }
    return out;
  }
  if (cmdlen == 0 && (in & 0xC0) != 0x40) return out;
  cmd[cmdlen++] = in;
  if (cmdlen == 6) { cmdlen = 0; if (!rd_multi || (cmd[0] & 0x3F) == 12) do_cmd(); }
  return out;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
  if (port != SD_CS_GPIO_Port || pin != SD_CS_Pin) return;
  cs_low = state == GPIO_PIN_RESET;
  if (!cs_low) { cmdlen = 0; qh = qt; wr_mode = 0; rd_multi = 0; }
}
static int spi_stuck;                  /* the SPI itself has stopped moving bytes */
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n, uint32_t t)
{
  (void)h; (void)t;
  if (spi_stuck) return HAL_TIMEOUT;
  for (int i = 0; i < n; i++) rx[i] = card_byte(tx[i]);
  return HAL_OK;
}
static void feed(int max);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *tx, uint16_t n, uint32_t t)
{
  (void)h; (void)t;
  for (int i = 0; i < n; i++) card_byte(tx[i]);
  /* CAN routing keeps running while the card is busy, so frames keep arriving while a
   * block is being written: the ring holds records stamped after the logger last read
   * the clock. */
  if (n == 512) feed(3);
  return HAL_OK;
}
/* On a PC no buffer address is in the STM32's DMA-reachable range, so these are never used. */
HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n)
{ (void)h; (void)tx; (void)rx; (void)n; return HAL_ERROR; }
HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *h, uint8_t *tx, uint16_t n)
{ (void)h; (void)tx; (void)n; return HAL_ERROR; }
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *h) { (void)h; return HAL_OK; }

/* ---------------- playing can_task: feed the ring between logger steps ---------------- */
typedef struct { logger_stats_t stats; uint32_t produced; } result_t;
static result_t *result;               /* shared with the parent */
static uint32_t target, produced;
static int inject_fault, fault_seen, idle_rounds;

/* Push up to max records into the ring, stamped with the current time. */
static void feed(int max)
{
  log_rec_t rec;
  uint8_t data[8];

  while (produced < target && max-- > 0) {
    memcpy(data, &produced, 4); memcpy(data + 4, "\x55\xAA\x55\xAA", 4);
    logrec_frame(&rec, produced % 4 ? LOGREC_RX : LOGREC_TX, timebase_now_lo(),
                 (uint16_t)(0x100 + produced % 256), (uint8_t)(produced % 9), 0, data);
    if (!ringbuf_push(&log_ring, &rec)) break;        /* full: wait (a real can_task would drop) */
    produced++;
  }
}

/* One main-loop pass apart from the logger: time moves on and frames arrive. */
static void other_jobs(uint32_t ms)
{
  logger_stats_t st;

  tick_ms += ms;
  logger_get_stats(&st);

  /* Fault scenario: the card starts rejecting writes part-way through, and recovers
   * once the logger has noticed. */
  if (inject_fault && !fault_seen && produced >= target / 2) reject_writes = 1;
  if (st.state == LOGGER_FAULT) { fault_seen = 1; reject_writes = 0; }

  feed(2000);

  /* Done once everything is fed, drained, and the once-a-second flush has had time to run. */
  if (st.state == LOGGER_CARD_FULL || (produced == target && ringbuf_count(&log_ring) == 0)) {
    if (++idle_rounds > 80) {
      assert(acmd23_bad == 0);                        /* every multi-block write was pre-erased */
      result->stats = st;
      result->produced = produced;
      _exit(0);
    }
  } else {
    idle_rounds = 0;
  }
}

/* One boot: run the logger in a child until the scenario finishes; return its final stats. */
#define STALE_RECORDS 5
static result_t boot(uint32_t records, int with_fault, int with_stale)
{
  int status;
  pid_t pid = fork();

  assert(pid >= 0);
  if (pid == 0) {
    target = records;
    inject_fault = with_fault;
    ringbuf_init(&log_ring, log_store, LOG_RING_RECORDS);
    if (with_stale) {
      /* Two hours after power-up (so times also pass 2^32 us), with records left over
       * from 31 minutes ago, before the card came up: too old to timestamp, so they must
       * be thrown away and reported, never written with a wrong time. */
      log_rec_t rec;
      tick_ms = 2u * 3600u * 1000u;
      for (int i = 0; i < STALE_RECORDS; i++) {
        logrec_time(&rec, timebase_now_lo() - 31u * 60u * 1000000u, 0);
        assert(ringbuf_push(&log_ring, &rec));
      }
    }
    logger_init();
    for (;;) {                                        /* the main loop; other_jobs() exits when done */
      other_jobs(20);
      logger_poll();
    }
  }
  assert(waitpid(pid, &status, 0) == pid);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return *result;
}

/* Copy a log file off the card into out/card/. Returns its size, 0 if it does not exist. */
static uint32_t pull(const char *name)
{
  static uint8_t buf[4096];
  char path[64];
  uint32_t total = 0;
  UINT got;
  FILE *out;

  snprintf(path, sizeof path, "LOG/%s", name);
  if (f_open(&USERFile, path, FA_READ) != FR_OK) return 0;
  snprintf(path, sizeof path, "out/card/%s", name);
  out = fopen(path, "wb");
  assert(out);
  while (f_read(&USERFile, buf, sizeof buf, &got) == FR_OK && got) { fwrite(buf, 1, got, out); total += got; }
  fclose(out);
  f_close(&USERFile);
  return total;
}

static int yields;
static void count_yield(void) { yields++; }

int main(void)
{
  static const SD_Hooks count_hooks = { count_yield, NULL };
  static uint8_t work[4096];
  const uint32_t part_size = LOG_PREALLOC_MB * 1024u * 1024u;
  uint32_t free_mb, max_boot;
  result_t r;

  img = mmap(NULL, (size_t)CARD_SECTORS * 512, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  result = mmap(NULL, sizeof *result, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  assert(img != MAP_FAILED && result != MAP_FAILED);
  mkdir("out", 0755); mkdir("out/card", 0755);

  /* A blank card, freshly formatted. */
  assert(MX_FATFS_Init() == APP_OK);
  assert(SD_Init() == SD_OK && SD_GetSectorCount() == CARD_SECTORS);

  /* Every loop in the driver that waits on the card must let other work run. */
  SD_SetHooks(&count_hooks);
  assert(SD_Init() == SD_OK && yields >= 2);          /* start-up: the card said "not ready" twice */
  yields = 0;
  read_delay = 50;                                    /* a card that is slow to produce read data */
  assert(SD_ReadBlocks(work, 0, 1) == SD_OK && yields > 0);
  read_delay = 0;

  /* A byte that times out must fail the operation. It must never be mistaken for the
   * 0xFF that means "card ready". And the driver must recover once the SPI works again. */
  spi_stuck = 1;
  assert(SD_Sync() != SD_OK);
  assert(SD_ReadBlocks(work, 0, 1) != SD_OK);
  assert(SD_WriteBlocks(work, 0, 1) != SD_OK);
  spi_stuck = 0;
  assert(SD_Sync() == SD_OK);
  assert(SD_ReadBlocks(work, 0, 1) == SD_OK);
  assert(f_mkfs(USERPath, FM_ANY, 0, work, sizeof work) == FR_OK);
  assert(acmd23_calls > 0 && acmd23_bad == 0);        /* multi-block writes announce their size */

  /* Boot 1: a short session. */
  r = boot(20000, 0, 0);
  assert(r.stats.state == LOGGER_LOGGING && r.stats.boot_id == 1 && r.stats.part == 1);
  assert(r.stats.lines == 20000 + 1 && r.stats.faults == 0);      /* + the "start" line */

  /* Boot 2: the boot ID comes from the files on the card (the backup register was lost). */
  r = boot(1000, 0, 0);
  assert(r.stats.boot_id == 2 && r.stats.part == 1 && r.stats.lines == 1001);

  /* Boot 3: enough data to fill a part and roll over into the next. */
  busy_len = 40;                                      /* a card slow enough that the driver has to wait */
  r = boot(260000, 0, 0);
  busy_len = 3;
  /* While one staging buffer was being written, the other was being filled. */
  assert(r.stats.lines_during_write > 1000);
  assert(r.stats.boot_id == 3 && r.stats.part == 2 && r.stats.faults == 0);
  assert(r.stats.lines == 260000 + 2);

  /* Boot 4: the card rejects writes for a while. The logger goes to FAULT, retries,
   * and carries on in a new part. Only the staged lines are lost. It also starts two
   * hours after power-up with stale records waiting (see boot()). */
  r = boot(60000, 1, 1);
  assert(r.stats.boot_id == 4 && r.stats.faults == 1 && r.stats.part == 2);
  assert(r.stats.stale_dropped == STALE_RECORDS);
  assert(r.stats.state == LOGGER_LOGGING && r.produced == 60000);

  /* Boot 5: under LOG_MIN_FREE_MB left, so logging refuses to start. */
  r = boot(100, 0, 0);
  assert(r.stats.state == LOGGER_CARD_FULL && r.stats.boot_id == 5 && r.stats.part == 0);

  /* The card must still mount, and every part must be exactly its preallocated size. */
  assert(storage_mount(&free_mb, &max_boot) == 0);
  assert(max_boot == 4 && free_mb < LOG_MIN_FREE_MB);
  assert(pull("B00001_01.jsonl") == part_size);
  assert(pull("B00002_01.jsonl") == part_size);
  assert(pull("B00003_01.jsonl") == part_size && pull("B00003_02.jsonl") == part_size);
  assert(pull("B00004_01.jsonl") == part_size && pull("B00004_02.jsonl") == part_size);
  assert(pull("B00005_01.jsonl") == 0);
  puts("logger       ok  (5 simulated boots: normal, boot ID, rollover, card fault, card full)");
  return 0;
}
