/**
  * logrec.h - the 16-byte binary record every event travels through RAM as.
  * Records become JSON only in the logger, right before writing (see jsonl.h).
  * No HAL here: this compiles and runs in the PC unit tests.
  */
#ifndef LOGREC_H
#define LOGREC_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
  uint32_t t_us;     /* low 32 bits of microseconds since boot */
  uint16_t id;       /* 11-bit CAN ID in bits 0-10 */
  uint8_t  dlc;      /* data length, 0-8 */
  uint8_t  type;     /* bits 0-3: record kind, bit 4: RTR */
  uint8_t  data[8];  /* payload, or the fields of a non-frame record */
} log_rec_t;

/* Record kinds (bits 0-3 of type) */
enum {
  LOGREC_RX = 0,   /* frame received */
  LOGREC_TX,       /* frame this board sent */
  LOGREC_TIME,     /* data: uint32 RTC Unix seconds */
  LOGREC_ERR,      /* data: tec, rec, lec, state */
  LOGREC_DROP,     /* data: uint32 frames lost */
  LOGREC_SYS,      /* data: event code (LOGREC_EV_*) */
  LOGREC_TSET,     /* data: uint32 old, uint32 new Unix seconds */
};
#define LOGREC_KIND_MASK 0x0F
#define LOGREC_RTR       0x10

/* CAN error states for LOGREC_ERR */
enum { LOGREC_ST_OK = 0, LOGREC_ST_WARN, LOGREC_ST_PASSIVE, LOGREC_ST_BUSOFF };

/* Event codes for LOGREC_SYS */
enum {
  LOGREC_EV_START = 0,   /* logging started on a new file part */
  LOGREC_EV_CARD_IN,
  LOGREC_EV_CARD_OUT,
  LOGREC_EV_POWERFAIL,
  LOGREC_EV_RECOVER,
  LOGREC_EV_FAULT,
  LOGREC_EV_BUSOFF_RESTART,
  LOGREC_EV_COUNT
};

void logrec_frame(log_rec_t *r, uint8_t kind, uint32_t t_us, uint16_t id,
                  uint8_t dlc, int rtr, const uint8_t *data);
void logrec_time(log_rec_t *r, uint32_t t_us, uint32_t rtc_unix);
void logrec_err(log_rec_t *r, uint32_t t_us, uint8_t tec, uint8_t rec, uint8_t lec, uint8_t state);
void logrec_drop(log_rec_t *r, uint32_t t_us, uint32_t count);
void logrec_sys(log_rec_t *r, uint32_t t_us, uint8_t event);
void logrec_tset(log_rec_t *r, uint32_t t_us, uint32_t old_unix, uint32_t new_unix);

/* Build an "rx" record from an FDCAN RX FIFO element (classic CAN, standard ID) as it
 * sits in the message RAM: e[0..1] header, e[2..3] the first 8 data bytes. A DLC of
 * 9-15 is legal in classic CAN and still means 8 bytes; at most 8 are ever copied.
 * Word reads only (the message RAM is word-wide). Pure code: tested on a PC. */
void logrec_rx_element(log_rec_t *r, uint32_t t_us, const volatile uint32_t *e);

/* Read a little-endian uint32 out of data[] at the given offset. */
uint32_t logrec_u32(const log_rec_t *r, int offset);

#endif /* LOGREC_H */
