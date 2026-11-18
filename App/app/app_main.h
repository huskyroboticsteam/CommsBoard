/**
  * app_main.h - what the application layer shares between its files, and the few
  * entry points the CubeMX-generated code calls (one line each, from USER CODE blocks).
  */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdint.h>
#include "ringbuf.h"

/* ---- Called from CubeMX-generated files ---- */
void app_main(void);        /* main.c, after the MX_..._Init calls, before the scheduler */
void app_rtos_init(void);   /* app_freertos.c, before the tasks are created */
void app_fatal(void);       /* stack overflow and similar: stop and let the watchdog reset us */
void can_task(void *argument);
void logger_task(void *argument);
void status_task(void *argument);

/* ---- CAN (can_task.c) ---- */
extern ringbuf_t log_ring;  /* binary records on their way to the card. Writer: can_task. Reader: logger_task. */

/* Queue a frame for sending. Any task may call this. 1 = queued, 0 = queue full. */
int can_send(uint16_t id, uint8_t dlc, const uint8_t *data, int rtr);

typedef struct {
  uint32_t log_dropped;     /* records lost because the log ring was full */
  uint32_t tx_queue_full;   /* can_send() calls refused */
} can_task_stats_t;
void can_task_get_stats(can_task_stats_t *out);

/* ---- Logger (logger.c) ---- */
typedef enum {
  LOGGER_INIT = 0,          /* starting the card and mounting it */
  LOGGER_OPEN,              /* creating the next file part */
  LOGGER_LOGGING,
  LOGGER_FAULT,             /* card failed; retried every SD_FAULT_RETRY_MS */
  LOGGER_CARD_FULL,
} logger_state_t;

typedef struct {
  logger_state_t state;
  uint32_t boot_id;
  uint32_t part;            /* file part being written */
  uint32_t lines;           /* JSON lines formatted this boot */
  uint64_t bytes;           /* bytes written to the card this boot */
  uint32_t faults;          /* times the logger entered FAULT */
} logger_stats_t;
void logger_get_stats(logger_stats_t *out);
void logger_wake(void);     /* can_task calls this when enough records are waiting */

/* ---- Watchdog check-ins (status.c) ---- */
enum { WDG_CAN = 1u << 0, WDG_LOGGER = 1u << 1, WDG_STATUS = 1u << 2, WDG_ALL = 7u };
void wdg_checkin(uint32_t task_bit);   /* each task calls this every loop */

#endif /* APP_MAIN_H */
