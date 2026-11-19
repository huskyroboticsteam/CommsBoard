/**
  * can_task.c - routes every CAN frame.
  *
  * The only code that touches the CAN transmit path, and the only writer into the log
  * ring. Each call to can_task_poll(): moves records from the driver's RX ring into the
  * log ring (received frames, frames of ours confirmed sent, error-state changes), hands
  * queued frames to the hardware, handles bus-off, and adds a "time" record once a
  * second. It never waits: if the log ring is full, the record is dropped and counted.
  *
  * It runs once per main-loop pass, and again from app_yield() whenever the SD driver is
  * waiting on the card, so it keeps running during a slow write.
  */
#include "app_main.h"
#include "board.h"
#include "config.h"
#include "can_drv.h"
#include "timebase.h"
#include "timekeeping.h"

static log_rec_t log_store[LOG_RING_RECORDS];
ringbuf_t log_ring;

static log_rec_t tx_store[CAN_TX_QUEUE_LEN + 1];    /* a ring keeps one slot empty */
static ringbuf_t tx_queue;                          /* writers: can_send(); reader: can_task_poll() */

static uint32_t pending_drop;       /* records lost since the last "drop" record */
static uint32_t log_dropped, tx_queue_full, tx_refused;

void can_task_init(void)
{
  ringbuf_init(&log_ring, log_store, LOG_RING_RECORDS);
  ringbuf_init(&tx_queue, tx_store, CAN_TX_QUEUE_LEN + 1);
  if (can_drv_init(CAN_BITRATE_BPS, CAN_LOOPBACK) != 0) app_fatal();
}

int can_send(uint16_t id, uint8_t dlc, const uint8_t *data, int rtr)
{
  log_rec_t rec;
  uint32_t irq;
  int ok;

  logrec_frame(&rec, LOGREC_TX, 0, id, dlc, rtr, data);
  irq = board_irq_lock();           /* callers may be the main loop or an interrupt */
  ok = ringbuf_push(&tx_queue, &rec);
  if (!ok) tx_queue_full++;
  board_irq_unlock(irq);
  return ok;
}

void can_task_get_stats(can_task_stats_t *out)
{
  out->log_dropped = log_dropped;
  out->tx_queue_full = tx_queue_full;
  out->tx_refused = tx_refused;
}

/* Put a record in the log ring. After an overflow, the first record that fits is
 * preceded by a "drop" record saying how many were lost. */
static void log_put(const log_rec_t *rec)
{
  log_rec_t drop;

  if (pending_drop) {
    logrec_drop(&drop, rec->t_us, pending_drop);
    if (!ringbuf_push(&log_ring, &drop)) {      /* still full */
      pending_drop++;
      log_dropped++;
      return;
    }
    pending_drop = 0;
  }
  if (!ringbuf_push(&log_ring, rec)) {
    pending_drop++;
    log_dropped++;
  }
}

#if TEST_GEN_FRAMES_PER_MS
/* Test aid: fake traffic, TEST_GEN_FRAMES_PER_MS frames for every millisecond that passed. */
static void test_gen(uint32_t now_ms)
{
  static uint32_t counter, gen_ms;
  uint8_t data[8];
  int i;

  if (now_ms - gen_ms > 5) gen_ms = now_ms - 5;     /* after a long stall, do not try to catch up */
  while (gen_ms != now_ms) {
    gen_ms++;
    for (i = 0; i < TEST_GEN_FRAMES_PER_MS; i++) {
      counter++;
      data[0] = (uint8_t)counter;         data[1] = (uint8_t)(counter >> 8);
      data[2] = (uint8_t)(counter >> 16); data[3] = (uint8_t)(counter >> 24);
      data[4] = 0x55; data[5] = 0xAA; data[6] = 0x55; data[7] = 0xAA;
      can_send((uint16_t)(0x100 + (counter & 0xFF)), 8, data, 0);
    }
  }
}
#endif

void can_task_poll(void)
{
  static uint32_t frames, rx_dropped_seen, last_time_ms;
  uint32_t now_ms = timebase_ms(), rx_dropped;
  log_rec_t rec;

#if TEST_GEN_FRAMES_PER_MS
  test_gen(now_ms);
#endif

  /* 1. Everything from the interrupt goes to the log: frames received, frames of ours
   *    confirmed sent, and error-state changes.
   *    (Chapter 7: frames addressed to this board also go to the CAN26 handlers here.) */
  while (can_drv_rx_pop(&rec)) {
    log_put(&rec);
    if ((++frames & 63) == 0) led_toggle(LED_CAN);
  }

  /* 2. Hand queued frames to the hardware, as long as its 3-slot FIFO has room. Each is
   *    logged as "tx" by step 1, once the driver reports it really went out. */
  while (can_drv_tx_free() && ringbuf_pop(&tx_queue, &rec)) {
    if (!can_drv_tx(&rec)) tx_refused++;            /* should not happen: we just checked for room */
  }

  /* 3. Bus-off: the driver restarts the controller after CAN_BUSOFF_RESTART_MS. */
  if (can_drv_service()) {
    logrec_sys(&rec, timebase_now_lo(), LOGREC_EV_BUSOFF_RESTART);
    log_put(&rec);
  }

  /* 4. Frames the interrupt had to drop count as lost records too. */
  rx_dropped = can_drv_rx_dropped();
  if (rx_dropped != rx_dropped_seen) {
    pending_drop += rx_dropped - rx_dropped_seen;
    rx_dropped_seen = rx_dropped;
  }

  /* 5. Wall-clock marker once a second. */
  if (now_ms - last_time_ms >= TIME_RECORD_INTERVAL_MS) {
    last_time_ms = now_ms;
    logrec_time(&rec, timebase_now_lo(), timebase_rtc_is_valid() ? tk_rtc_unix() : 0);
    log_put(&rec);
  }
}
