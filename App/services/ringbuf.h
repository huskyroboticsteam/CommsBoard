/**
  * ringbuf.h - fixed-size ring of log records for exactly ONE writer and ONE reader.
  *
  * The writer only changes head and the reader only changes tail, so no lock is
  * needed between an interrupt and the main loop. One slot always
  * stays empty, so a ring made with cap slots holds cap - 1 records.
  * No HAL here: this compiles and runs in the PC unit tests.
  */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdint.h>
#include "logrec.h"

typedef struct {
  log_rec_t *buf;
  uint32_t cap;             /* number of slots */
  volatile uint32_t head;   /* next slot to write (writer only) */
  volatile uint32_t tail;   /* next slot to read (reader only) */
  uint32_t high_water;      /* most records ever waiting (writer only) */
} ringbuf_t;

void ringbuf_init(ringbuf_t *r, log_rec_t *storage, uint32_t cap);
int ringbuf_push(ringbuf_t *r, const log_rec_t *rec);   /* 1 = stored, 0 = full (never waits) */
int ringbuf_pop(ringbuf_t *r, log_rec_t *rec);          /* 1 = got one, 0 = empty */
uint32_t ringbuf_count(const ringbuf_t *r);

/* Zero-copy variants, for hot paths that build a record in place.
 * Writer: ringbuf_slot() returns the next free slot (NULL = full); fill it, then
 * ringbuf_commit() publishes it. Nothing is visible to the reader until the commit.
 * Reader: ringbuf_peek() returns the oldest record without removing it (NULL = empty);
 * ringbuf_drop() removes it. */
log_rec_t *ringbuf_slot(ringbuf_t *r);
void ringbuf_commit(ringbuf_t *r);
const log_rec_t *ringbuf_peek(const ringbuf_t *r);
void ringbuf_drop(ringbuf_t *r);

#endif /* RINGBUF_H */
