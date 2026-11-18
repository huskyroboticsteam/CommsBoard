/* can_drv.c - FDCAN2 driver. See can_drv.h. */
#include "can_drv.h"
#include "board.h"
#include "config.h"
#include "fdcan.h"
#include "ringbuf.h"
#include "timebase.h"

#define TQ_PER_BIT  17      /* 1 sync + 14 + 2: sample point at 15/17 = 88% */

static log_rec_t rx_store[CAN_RX_RING_FRAMES];
static ringbuf_t rx_ring;                 /* writer: the FDCAN interrupt; reader: can_task */
static void (*notify)(void);

static volatile uint32_t rx_frames, rx_dropped, tx_frames, busoff_recoveries;
static volatile uint8_t busoff_pending;
static volatile uint32_t busoff_tick;

int can_drv_init(uint32_t bitrate, int loopback, void (*notify_isr)(void))
{
  FDCAN_HandleTypeDef *h = &BOARD_CAN_HANDLE;
  uint32_t clk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN);

  notify = notify_isr;
  ringbuf_init(&rx_ring, rx_store, CAN_RX_RING_FRAMES);

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
  if (h->Init.NominalPrescaler == 0 || HAL_FDCAN_Init(h) != HAL_OK) return -1;

  /* No filters: every standard-ID frame (data or remote) goes to RX FIFO 0.
   * The board logs everything; CAN26 decides what it acts on. Extended IDs are not used. */
  if (HAL_FDCAN_ConfigGlobalFilter(h, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT,
                                   FDCAN_FILTER_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK) return -1;

  if (HAL_FDCAN_ActivateNotification(h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST
                                      | FDCAN_IT_ERROR_WARNING | FDCAN_IT_ERROR_PASSIVE | FDCAN_IT_BUS_OFF, 0) != HAL_OK) return -1;
  if (HAL_FDCAN_ActivateNotification(h, FDCAN_IT_TX_COMPLETE,
                                     FDCAN_TX_BUFFER0 | FDCAN_TX_BUFFER1 | FDCAN_TX_BUFFER2) != HAL_OK) return -1;
  return (HAL_FDCAN_Start(h) == HAL_OK) ? 0 : -1;
}

int can_drv_rx_pop(log_rec_t *rec)
{
  return ringbuf_pop(&rx_ring, rec);
}

int can_drv_tx_free(void)
{
  return (int)HAL_FDCAN_GetTxFifoFreeLevel(&BOARD_CAN_HANDLE);
}

int can_drv_tx(const log_rec_t *frame)
{
  FDCAN_HandleTypeDef *h = &BOARD_CAN_HANDLE;
  FDCAN_TxHeaderTypeDef hdr = {0};

  if (HAL_FDCAN_GetTxFifoFreeLevel(h) == 0) return 0;
  hdr.Identifier = frame->id;
  hdr.IdType = FDCAN_STANDARD_ID;
  hdr.TxFrameType = (frame->type & LOGREC_RTR) ? FDCAN_REMOTE_FRAME : FDCAN_DATA_FRAME;
  hdr.DataLength = frame->dlc;            /* 0-8 maps straight onto the DLC code */
  hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  hdr.BitRateSwitch = FDCAN_BRS_OFF;
  hdr.FDFormat = FDCAN_CLASSIC_CAN;
  hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  if (HAL_FDCAN_AddMessageToTxFifoQ(h, &hdr, (uint8_t *)frame->data) != HAL_OK) return 0;
  tx_frames++;
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
  FDCAN_ProtocolStatusTypeDef ps;
  FDCAN_ErrorCountersTypeDef ec;

  HAL_FDCAN_GetProtocolStatus(&BOARD_CAN_HANDLE, &ps);
  HAL_FDCAN_GetErrorCounters(&BOARD_CAN_HANDLE, &ec);
  out->rx_frames = rx_frames;
  out->rx_dropped = rx_dropped;
  out->tx_frames = tx_frames;
  out->busoff_recoveries = busoff_recoveries;
  out->rx_ring_high = rx_ring.high_water;
  out->tec = (uint8_t)ec.TxErrorCnt;
  out->rec = (uint8_t)ec.RxErrorCnt;
  out->state = error_state(&ps);
}

/* ---- Interrupt side (HAL calls these from FDCAN2_IT0_IRQHandler) ---- */

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
  uint32_t t = timebase_now_lo();     /* read the clock first: keeps timestamp jitter small */
  FDCAN_RxHeaderTypeDef hdr;
  uint8_t data[8];
  log_rec_t rec;

  if (RxFifo0ITs & FDCAN_IT_RX_FIFO0_MESSAGE_LOST) rx_dropped++;   /* hardware FIFO overflowed */

  while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) > 0) {
    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &hdr, data) != HAL_OK) break;
    logrec_frame(&rec, LOGREC_RX, t, (uint16_t)hdr.Identifier, (uint8_t)hdr.DataLength,
                 hdr.RxFrameType == FDCAN_REMOTE_FRAME, data);
    if (ringbuf_push(&rx_ring, &rec)) rx_frames++;
    else rx_dropped++;                /* ring full: count it, never wait */
  }
  if (notify) notify();
}

void HAL_FDCAN_TxBufferCompleteCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t BufferIndexes)
{
  (void)hfdcan; (void)BufferIndexes;
  if (notify) notify();               /* TX space freed: the task can send the next frame */
}

void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
{
  FDCAN_ProtocolStatusTypeDef ps;
  FDCAN_ErrorCountersTypeDef ec;
  log_rec_t rec;

  (void)ErrorStatusITs;
  HAL_FDCAN_GetProtocolStatus(hfdcan, &ps);
  HAL_FDCAN_GetErrorCounters(hfdcan, &ec);
  logrec_err(&rec, timebase_now_lo(), (uint8_t)ec.TxErrorCnt, (uint8_t)ec.RxErrorCnt,
             (uint8_t)ps.LastErrorCode, error_state(&ps));
  if (!ringbuf_push(&rx_ring, &rec)) rx_dropped++;

  if (ps.BusOff && !busoff_pending) {
    busoff_tick = HAL_GetTick();
    busoff_pending = 1;               /* can_drv_service() restarts us after the delay */
  }
  if (notify) notify();
}
