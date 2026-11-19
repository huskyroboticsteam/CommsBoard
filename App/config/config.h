/**
  * config.h - every tunable number in the firmware (architecture v2, section 10).
  * Measurements from the dev board should only ever change this file.
  */
#ifndef CONFIG_H
#define CONFIG_H

#include "board.h"

/* ---- CAN ---- */
#define CAN_BITRATE_BPS           1000000   /* confirm against CAN26 */
#ifdef BOARD_NUCLEO
#define CAN_LOOPBACK              1         /* internal loopback: no transceiver or second node needed */
#else
#define CAN_LOOPBACK              0         /* the real bus. Never loopback on the rover: nothing would reach it */
#endif
#define CAN_TX_QUEUE_LEN          32
#define CAN_RX_RING_FRAMES        512
#define CAN_BUSOFF_RESTART_MS     100

/* ---- Logging ---- */
#define LOG_RING_RECORDS          3072      /* 48 KB of 16 B records; resize from measured SD stalls */
#define JSON_STAGING_BYTES        4096      /* multiple of 512 */
#define LOG_FLUSH_INTERVAL_MS     1000      /* max data lost on a power cut */
#define TIME_RECORD_INTERVAL_MS   1000
#ifdef BOARD_NUCLEO
/* The dev card is 2 GB, so the spec sizes (1 GB per part, 2 GB free) can never fit.
 * Small parts also exercise the "next part" path every couple of minutes. */
#define LOG_PREALLOC_MB           16
#define LOG_MIN_FREE_MB           64
#else
#define LOG_PREALLOC_MB           1024      /* per file part */
#define LOG_MIN_FREE_MB           2048
#endif

/* ---- SD card ---- */
#define SD_SPI_INIT_HZ            332000    /* APB2 85 MHz / 256; must be 400 kHz or less */
#define SD_SPI_FAST_HZ            21250000  /* APB2 85 MHz / 4 */
#define SD_WRITE_TIMEOUT_MS       500
#define SD_MAX_RECOVERIES         3         /* tries before FAULT */
#define SD_FAULT_RETRY_MS         10000
#define SD_INSERT_DEBOUNCE_MS     200

/* ---- Power-fail (chapter 6) ---- */
#define PF_TRIP_INPUT_MV          4500      /* input voltage that triggers shutdown */
#define PF_DIVIDER_RATIO          0.5f      /* from the schematic */

/* ---- Status, watchdog ---- */
#define STATS_PRINT_MS            5000
#define WATCHDOG_TIMEOUT_MS       2000      /* set by the IWDG prescaler/reload in CubeMX */

/* ---- Test aids ---- */
/* Fake CAN frames sent per millisecond in loopback (0 = off). 2 = 2,000 frames/s, 8 = 8,000. */
#define TEST_GEN_FRAMES_PER_MS    0

#endif /* CONFIG_H */
