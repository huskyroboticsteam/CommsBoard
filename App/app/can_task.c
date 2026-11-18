/**
  * can_task.c - routes every CAN frame.
  *
  * The only task that touches the CAN transmit path, and the only writer into the log
  * ring. Each loop it: moves records from the driver's RX ring into the log ring,
  * sends queued frames (logging each as "tx"), handles bus-off, and adds a "time"
  * record once a second. It never waits on the SD card: if the log ring is full, the
  * record is dropped and counted.
  */
#include "app_main.h"
#include "board.h"
#include "config.h"
#include "can_drv.h"
#include "timebase.h"
#include "timekeeping.h"
#include "cmsis_os.h"

#define FLAG_WORK           0x1u
#define LOOP_TIMEOUT_MS     50      /* wake at least this often: time records, watchdog */
#define LOGGER_WAKE_RECORDS 32      /* wake the logger once this many records are waiting */

extern osThreadId_t canTaskHandle;

static log_rec_t log_store[LOG_RING_RECORDS];
ringbuf_t log_ring;

static osMessageQueueId_t tx_queue;
static uint32_t pending_drop;       /* records lost since the last "drop" record */
static uint32_t log_dropped, tx_queue_full;

static void wake_task(void)         /* safe from interrupts and tasks */
{
  osThreadFlagsSet(canTaskHandle, FLAG_WORK);
}

void app_rtos_init(void)
{
  ringbuf_init(&log_ring, log_store, LOG_RING_RECORDS);
  tx_queue = osMessageQueueNew(CAN_TX_QUEUE_LEN, sizeof(log_rec_t), NULL);
}

int can_send(uint16_t id, uint8_t dlc, const uint8_t *data, int rtr)
{
  log_rec_t rec;

  logrec_frame(&rec, LOGREC_TX, 0, id, dlc, rtr, data);
  if (osMessageQueuePut(tx_queue, &rec, 0, 0) != osOK) {
    tx_queue_full++;
    return 0;
  }
  wake_task();
  return 1;
}

void can_task_get_stats(can_task_stats_t *out)
{
  out->log_dropped = log_dropped;
  out->tx_queue_full = tx_queue_full;
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
/* Test aid: fake traffic, TEST_GEN_FRAMES_PER_MS frames every millisecond. */
static void test_gen(void *argument)
{
  static uint32_t counter;
  uint8_t data[8];
  int i;

  (void)argument;
  for (i = 0; i < TEST_GEN_FRAMES_PER_MS; i++) {
    counter++;
    data[0] = (uint8_t)counter;        data[1] = (uint8_t)(counter >> 8);
    data[2] = (uint8_t)(counter >> 16); data[3] = (uint8_t)(counter >> 24);
    data[4] = 0x55; data[5] = 0xAA; data[6] = 0x55; data[7] = 0xAA;
    can_send((uint16_t)(0x100 + (counter & 0xFF)), 8, data, 0);
  }
}
#endif

void can_task(void *argument)
{
  log_rec_t rec;
  uint32_t frames = 0, rx_dropped_seen = 0, last_time_ms = 0, now_ms;
  can_stats_t cs;

  (void)argument;
  if (can_drv_init(CAN_BITRATE_BPS, CAN_LOOPBACK, wake_task) != 0) app_fatal();

#if TEST_GEN_FRAMES_PER_MS
  osTimerStart(osTimerNew(test_gen, osTimerPeriodic, NULL, NULL), 1);
#endif

  for (;;) {
    osThreadFlagsWait(FLAG_WORK, osFlagsWaitAny, LOOP_TIMEOUT_MS);

    /* 1. Everything the interrupt received goes to the log.
     *    (Chapter 7: frames addressed to this board also go to the CAN26 handlers here.) */
    while (can_drv_rx_pop(&rec)) {
      log_put(&rec);
      if ((++frames & 63) == 0) led_toggle(LED_CAN);
    }

    /* 2. Send what other tasks queued, as long as the hardware has room. */
    /*    When the 3-slot hardware FIFO is full, the TX-complete interrupt wakes us again. */
    while (can_drv_tx_free() && osMessageQueueGet(tx_queue, &rec, NULL, 0) == osOK) {
      if (!can_drv_tx(&rec)) continue;
      rec.t_us = timebase_now_lo();
      log_put(&rec);
    }

    /* 3. Bus-off: the driver restarts the controller after CAN_BUSOFF_RESTART_MS. */
    if (can_drv_service()) {
      logrec_sys(&rec, timebase_now_lo(), LOGREC_EV_BUSOFF_RESTART);
      log_put(&rec);
    }

    /* 4. Frames the interrupt had to drop count as lost records too. */
    can_drv_get_stats(&cs);
    if (cs.rx_dropped != rx_dropped_seen) {
      pending_drop += cs.rx_dropped - rx_dropped_seen;
      rx_dropped_seen = cs.rx_dropped;
    }

    /* 5. Wall-clock marker once a second. */
    now_ms = osKernelGetTickCount();
    if (now_ms - last_time_ms >= TIME_RECORD_INTERVAL_MS) {
      last_time_ms = now_ms;
      logrec_time(&rec, timebase_now_lo(), timebase_rtc_is_valid() ? tk_rtc_unix() : 0);
      log_put(&rec);
    }

    if (ringbuf_count(&log_ring) >= LOGGER_WAKE_RECORDS) logger_wake();
    wdg_checkin(WDG_CAN);
  }
}
