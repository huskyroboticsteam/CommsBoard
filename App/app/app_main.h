/**
  * app_main.h - what the application layer shares between its files, and the entry
  * points the CubeMX-generated main.c calls (one line each, from USER CODE blocks).
  *
  * There is no RTOS. main() calls app_loop() forever, and each pass runs three jobs in
  * turn: CAN routing, the logger, and status. A job never sleeps. The one place that
  * has to wait is the SD card, and while it waits the SD driver calls app_yield(), which
  * runs the CAN and status jobs. That is how a slow card write never stalls CAN.
  */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdint.h>
#include "ringbuf.h"

/* ---- Called from main.c ---- */
void app_main(void);        /* once, after the MX_..._Init calls */
void app_loop(void);        /* one pass of the main loop */

/* Run the jobs that must not wait for the SD card (CAN routing and status).
 *
 * RULE: this is called from deep inside the SD driver, in the middle of a FatFs call.
 * Nothing it runs may call into FatFs, storage, the SD driver or the logger - FatFs
 * would re-enter itself and corrupt the card. Anything can_task_poll() or status_poll()
 * ever wants written to the card must go through the log ring.
 * The one exception is reading counters: SD_GetStats() and logger_get_stats() only copy
 * numbers and start nothing, so status_poll() may call them.
 * (The logger's own wait hook also formats records into its second buffer during a
 * write. That only touches the log ring and RAM, never the card.) */
void app_yield(void);

/* Longest time between two runs of the CAN job since this was last called (us). */
uint32_t app_can_gap_max_us(void);

void app_fatal(void);       /* stop and let the watchdog reset us */

/* ---- CAN routing (can_task.c) ---- */
extern ringbuf_t log_ring;  /* binary records on their way to the card. Writer: can_task_poll. Reader: logger_poll. */

void can_task_init(void);
void can_task_poll(void);

/* Queue a frame for sending. Safe from the main loop and from interrupts.
 * 1 = queued, 0 = queue full. */
int can_send(uint16_t id, uint8_t dlc, const uint8_t *data, int rtr);

typedef struct {
  uint32_t log_dropped;     /* records lost because the log ring was full */
  uint32_t tx_queue_full;   /* can_send() calls refused: the queue was full */
  uint32_t tx_refused;      /* frames the hardware would not take (lost) */
} can_task_stats_t;
void can_task_get_stats(can_task_stats_t *out);

/* ---- Logger (logger.c) ---- */
typedef enum {
  LOGGER_INIT = 0,          /* starting the card and mounting it */
  LOGGER_OPEN,              /* creating the next file part */
  LOGGER_LOGGING,
  LOGGER_FAULT,             /* card failed; retried every SD_FAULT_RETRY_MS */
  LOGGER_CARD_FULL,         /* no room for a new part (or out of part numbers); logging has stopped */
} logger_state_t;

typedef struct {
  logger_state_t state;
  uint32_t boot_id;
  uint32_t part;            /* file part being written */
  uint32_t lines;           /* JSON lines formatted this boot */
  uint32_t lines_during_write;  /* of those, formatted into the second buffer while the first was being written */
  uint64_t bytes;           /* bytes written to the card this boot */
  uint32_t faults;          /* times the logger entered FAULT */
  uint32_t stale_dropped;   /* records thrown away unwritten because they waited too long (no card) */
} logger_stats_t;

void logger_init(void);
void logger_poll(void);
void logger_get_stats(logger_stats_t *out);

/* ---- Status and watchdog (status.c) ---- */
void status_poll(void);
void wdg_checkin(void);     /* "still making progress": a finished loop pass, or the card answering */

#endif /* APP_MAIN_H */
