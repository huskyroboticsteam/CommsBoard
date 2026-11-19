/**
  * can_drv.h - FDCAN2 in classic CAN mode.
  *
  * Receive: every standard-ID frame on the bus is accepted. The interrupt timestamps
  * each frame and copies it into the RX ring as a 16-byte record; it never waits.
  * Error-state changes arrive in the same ring as "err" records.
  * Transmit: can_drv_tx() puts one frame into the 3-slot hardware FIFO. When the
  * controller reports that the frame really went out on the bus (it was acknowledged),
  * a "tx" record appears in the RX ring. A frame nobody acknowledges is retried by the
  * hardware and never logged as sent.
  */
#ifndef CAN_DRV_H
#define CAN_DRV_H

#include <stdint.h>
#include "logrec.h"

typedef struct {
  uint32_t rx_frames;         /* frames copied into the RX ring */
  uint32_t rx_dropped;        /* records lost: RX ring full, or a hardware FIFO overflowed. A lower
                                 bound: the hardware reports an overflow, not how many frames it cost. */
  uint32_t tx_frames;         /* frames confirmed sent on the bus */
  uint32_t busoff_recoveries;
  uint32_t rx_ring_high;      /* most records ever waiting in the RX ring */
  uint8_t  tec, rec;          /* error counters right now */
  uint8_t  state;             /* LOGREC_ST_* as of the last error-state interrupt */
} can_stats_t;

/* Configure and start the controller. Returns 0 on success. */
int can_drv_init(uint32_t bitrate, int loopback);

int can_drv_rx_pop(log_rec_t *rec);        /* 1 = got a record (rx, tx or err) from the RX ring */
uint32_t can_drv_rx_dropped(void);         /* running count of lost records; cheap to call */
int can_drv_tx_free(void);                 /* free slots in the hardware TX FIFO (0-3) */
int can_drv_tx(const log_rec_t *frame);    /* 1 = queued in the hardware FIFO, 0 = refused */
int can_drv_service(void);                 /* call often; 1 = just restarted after bus-off */
void can_drv_get_stats(can_stats_t *out);

#endif /* CAN_DRV_H */
