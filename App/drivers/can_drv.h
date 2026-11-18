/**
  * can_drv.h - FDCAN2 in classic CAN mode.
  *
  * Receive: every standard-ID frame on the bus is accepted. The interrupt timestamps
  * each frame and copies it into the RX ring as a 16-byte record; it never waits.
  * Error-state changes arrive in the same ring as "err" records.
  * Transmit: can_drv_tx() puts one frame into the 3-slot hardware FIFO.
  */
#ifndef CAN_DRV_H
#define CAN_DRV_H

#include <stdint.h>
#include "logrec.h"

typedef struct {
  uint32_t rx_frames;         /* frames copied into the RX ring */
  uint32_t rx_dropped;        /* frames lost: RX ring full, or the hardware FIFO overflowed */
  uint32_t tx_frames;         /* frames handed to the hardware */
  uint32_t busoff_recoveries;
  uint32_t rx_ring_high;      /* most records ever waiting in the RX ring */
  uint8_t  tec, rec;          /* error counters right now */
  uint8_t  state;             /* LOGREC_ST_* right now */
} can_stats_t;

/* Configure and start the controller. notify_isr is called from the interrupt whenever
 * the task should run (frames waiting, TX space freed, bus-off). Returns 0 on success. */
int can_drv_init(uint32_t bitrate, int loopback, void (*notify_isr)(void));

int can_drv_rx_pop(log_rec_t *rec);        /* 1 = got a record from the RX ring */
int can_drv_tx_free(void);                 /* free slots in the hardware TX FIFO (0-3) */
int can_drv_tx(const log_rec_t *frame);    /* 1 = sent to the hardware FIFO, 0 = FIFO full */
int can_drv_service(void);                 /* call often; 1 = just restarted after bus-off */
void can_drv_get_stats(can_stats_t *out);

#endif /* CAN_DRV_H */
