/* can_drv.c - FDCAN2 driver. See can_drv.h. */
#include "can_drv.h"
#include "board.h"
#include "config.h"
#include "fdcan.h"
#include "ringbuf.h"
#include "timebase.h"

#define TQ_PER_BIT  17      /* 1 sync + 14 + 2: sample point at 15/17 = 88% */
#define TX_INFLIGHT 8       /* > 3 TX FIFO slots + 3 TX event slots */

static log_rec_t rx_store[CAN_RX_RING_FRAMES];
static ringbuf_t rx_ring;                 /* writer: the FDCAN interrupt; reader: can_task_poll() */

/* Frames handed to the hardware, kept until the controller reports them sent. The TX
 * event only carries the ID and a marker, so the marker indexes this table. */
static log_rec_t tx_inflight[TX_INFLIGHT];
static uint8_t tx_marker;

static volatile uint32_t rx_frames, rx_dropped, tx_frames, busoff_recoveries;
static volatile uint8_t bus_state;        /* LOGREC_ST_*, updated only by the interrupt */
static volatile uint8_t busoff_pending;
static volatile uint32_t busoff_tick;

int can_drv_init(uint32_t bitrate, int loopback)
{
  FDCAN_HandleTypeDef *h = &BOARD_CAN_HANDLE;
  uint32_t clk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN);

  ringbuf_init(&rx_ring, rx_store, CAN_RX_RING_FRAMES);

  /* The clock must divide into exactly 17 time quanta per bit, or the bit rate is off. */
  if (bitrate == 0 || clk % (TQ_PER_BIT * bitrate) != 0) return -1;

  /* CubeMX already initialised the peripheral; redo it with the bit rate from config.h. */
  h->Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  h->Init.Mode = loopback ? FDCAN_MODE_INTERNAL_LOOPBACK : FDCAN_MODE_NORMAL;
  h->Init.AutoRetransmission = ENABLE;
  h->Init.NominalPrescaler = clk / (TQ_PER_BIT * bitrate);
  h->Init.NominalSyncJumpWidth = 2;
  h->Init.NominalTimeSeg1 = 14;
  h->Init.NominalTimeSeg2 = 2;
  h->Init.StdFiltersNbr = 0;
  h->Init.ExtFiltersNbr = 0;
  h->Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  if (HAL_FDCAN_Init(h) != HAL_OK) return -1;

  /* No filters: every standard-ID frame (data or remote) goes to RX FIFO 0.
   * The board logs everything; CAN26 decides what it acts on. Extended IDs are not used. */
  if (HAL_FDCAN_ConfigGlobalFilter(h, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT,
                                   FDCAN_FILTER_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK) return -1;

  if (HAL_FDCAN_ActivateNotification(h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST
                                      | FDCAN_IT_TX_EVT_FIFO_NEW_DATA | FDCAN_IT_TX_EVT_FIFO_ELT_LOST
                                      | FDCAN_IT_ERROR_WARNING | FDCAN_IT_ERROR_PASSIVE | FDCAN_IT_BUS_OFF, 0) != HAL_OK) return -1;
  return (HAL_FDCAN_Start(h) == HAL_OK) ? 0 : -1;
}

int can_drv_rx_pop(log_rec_t *rec)
{
  return ringbuf_pop(&rx_ring, rec);
}

uint32_t can_drv_rx_dropped(void)
{
  return rx_dropped;
}

int can_drv_tx_free(void)
{
  return (int)HAL_FDCAN_GetTxFifoFreeLevel(&BOARD_CAN_HANDLE);
}

int can_drv_tx(const log_rec_t *frame)
{
  FDCAN_HandleTypeDef *h = &BOARD_CAN_HANDLE;
  FDCAN_TxHeaderTypeDef hdr = {0};
  uint8_t marker = tx_marker;

  if (HAL_FDCAN_GetTxFifoFreeLevel(h) == 0) return 0;
  tx_inflight[marker] = *frame;           /* before the frame is queued: its TX event may come at once */
  hdr.Identifier = frame->id;
  hdr.IdType = FDCAN_STANDARD_ID;
  hdr.TxFrameType = (frame->type & LOGREC_RTR) ? FDCAN_REMOTE_FRAME : FDCAN_DATA_FRAME;
  hdr.DataLength = frame->dlc;            /* 0-8 maps straight onto the DLC code */
  hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  hdr.BitRateSwitch = FDCAN_BRS_OFF;
  hdr.FDFormat = FDCAN_CLASSIC_CAN;
  hdr.TxEventFifoControl = FDCAN_STORE_TX_EVENTS;   /* tell us when it has really been sent */
  hdr.MessageMarker = marker;
  if (HAL_FDCAN_AddMessageToTxFifoQ(h, &hdr, (uint8_t *)frame->data) != HAL_OK) return 0;
  tx_marker = (uint8_t)((marker + 1) % TX_INFLIGHT);
  return 1;
}

int can_drv_service(void)
{
  if (!busoff_pending || HAL_GetTick() - busoff_tick < CAN_BUSOFF_RESTART_MS) return 0;
  busoff_pending = 0;
  /* Bus-off parks the controller in init mode; leaving it starts the recovery sequence. */
  CLEAR_BIT(BOARD_CAN_HANDLE.Instance->CCCR, FDCAN_CCCR_INIT);
  busoff_recoveries++;
  return 1;
}

static uint8_t error_state(const FDCAN_ProtocolStatusTypeDef *ps)
{
  if (ps->BusOff) return LOGREC_ST_BUSOFF;
  if (ps->ErrorPassive) return LOGREC_ST_PASSIVE;
  if (ps->Warning) return LOGREC_ST_WARN;
  return LOGREC_ST_OK;
}

void can_drv_get_stats(can_stats_t *out)
{
  FDCAN_ErrorCountersTypeDef ec;

  /* The error counters are safe to read here. The protocol status register is not:
   * reading it clears the last error code, so only the interrupt reads it. */
  HAL_FDCAN_GetErrorCounters(&BOARD_CAN_HANDLE, &ec);
  out->rx_frames = rx_frames;
  out->rx_dropped = rx_dropped;
  out->tx_frames = tx_frames;
  out->busoff_recoveries = busoff_recoveries;
  out->rx_ring_high = rx_ring.high_water;
  out->tec = (uint8_t)ec.TxErrorCnt;
  out->rec = (uint8_t)ec.RxErrorCnt;
  out->state = bus_state;
}

/* ---- Interrupt side (HAL calls these from FDCAN2_IT0_IRQHandler) ---- */

#define RXE_BYTES  (18u * 4u)   /* RX FIFO element size in the message RAM: 2 header words + 64 data bytes */

/* Drain RX FIFO 0 straight into the RX ring.
 *
 * This reads the message RAM directly instead of calling HAL_FDCAN_GetRxMessage(), for
 * two reasons:
 *  - Safety. The HAL copies DLCtoBytes[DLC] bytes, up to 64, whatever the frame format.
 *    A classic frame may legally carry a DLC of 9-15 (it still means 8 bytes), and the
 *    HAL would then write up to 64 bytes into the caller's buffer. logrec_rx_element()
 *    never takes more than 8.
 *  - Speed. Each frame is built once, in its ring slot: no header struct, no staging
 *    buffer, no extra 16-byte copy. This runs for every frame on the bus.
 * Only standard IDs reach this FIFO: the global filter rejects extended ones. */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
  FDCAN_GlobalTypeDef *can = hfdcan->Instance;
  uint32_t t = timebase_now_lo();     /* read the clock first: keeps timestamp jitter small */
  uint32_t status, get;
  log_rec_t *rec;

  /* The hardware FIFO overflowed. It reports the event, not how many frames went. */
  if (RxFifo0ITs & FDCAN_IT_RX_FIFO0_MESSAGE_LOST) rx_dropped++;

  while ((status = can->RXF0S) & FDCAN_RXF0S_F0FL) {
    get = (status & FDCAN_RXF0S_F0GI) >> FDCAN_RXF0S_F0GI_Pos;
    rec = ringbuf_slot(&rx_ring);
    if (rec) {
      logrec_rx_element(rec, t, (const volatile uint32_t *)(hfdcan->msgRam.RxFIFO0SA + get * RXE_BYTES));
      ringbuf_commit(&rx_ring);
      rx_frames++;
    } else {
      rx_dropped++;                   /* ring full: count it, never wait */
    }
    can->RXF0A = get;                 /* release the element, stored or not */
  }
}

/* A frame of ours made it onto the bus (it was acknowledged). Only now does it become
 * a "tx" record, stamped with the time it was sent. */
void HAL_FDCAN_TxEventFifoCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t TxEventFifoITs)
{
  uint32_t t = timebase_now_lo();
  FDCAN_TxEventFifoTypeDef ev;
  log_rec_t rec;

  if (TxEventFifoITs & FDCAN_IT_TX_EVT_FIFO_ELT_LOST) rx_dropped++;   /* a "tx" record we never saw */

  while (HAL_FDCAN_GetTxEvent(hfdcan, &ev) == HAL_OK) {
    rec = tx_inflight[ev.MessageMarker % TX_INFLIGHT];
    rec.t_us = t;
    tx_frames++;
    if (!ringbuf_push(&rx_ring, &rec)) rx_dropped++;
  }
}

void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
{
  FDCAN_ProtocolStatusTypeDef ps;
  FDCAN_ErrorCountersTypeDef ec;
  log_rec_t rec;

  (void)ErrorStatusITs;
  HAL_FDCAN_GetProtocolStatus(hfdcan, &ps);   /* the only place this register is read */
  HAL_FDCAN_GetErrorCounters(hfdcan, &ec);
  bus_state = error_state(&ps);
  logrec_err(&rec, timebase_now_lo(), (uint8_t)ec.TxErrorCnt, (uint8_t)ec.RxErrorCnt,
             (uint8_t)ps.LastErrorCode, bus_state);
  if (!ringbuf_push(&rx_ring, &rec)) rx_dropped++;

  if (ps.BusOff && !busoff_pending) {
    busoff_tick = HAL_GetTick();
    busoff_pending = 1;               /* can_drv_service() restarts us after the delay */
  }
}
