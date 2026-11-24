/* ringbuf.c - single-writer, single-reader ring of log records. See ringbuf.h. */
#include "ringbuf.h"

/* Compiler barrier. The CPU is a single in-order core, so no hardware fence is needed,
 * but the compiler must not move a slot's data accesses across the head/tail update
 * that hands the slot to the other side. */
#define RB_BARRIER() __asm volatile ("" ::: "memory")

void ringbuf_init(ringbuf_t *r, log_rec_t *storage, uint32_t cap)
{
  r->buf = storage;
  r->cap = cap;
  r->head = 0;
  r->tail = 0;
  r->high_water = 0;
}

uint32_t ringbuf_count(const ringbuf_t *r)
{
  uint32_t head = r->head, tail = r->tail;
  return (head >= tail) ? (head - tail) : (r->cap - tail + head);
}

static inline uint32_t next_index(const ringbuf_t *r, uint32_t i)
{
  return (i + 1 == r->cap) ? 0 : i + 1;
}

log_rec_t *ringbuf_slot(ringbuf_t *r)
{
  uint32_t head = r->head;

  return (next_index(r, head) == r->tail) ? 0 : &r->buf[head];   /* full: no slot */
}

void ringbuf_commit(ringbuf_t *r)
{
  uint32_t n;

  RB_BARRIER();                       /* the record is complete in memory... */
  r->head = next_index(r, r->head);   /* ...before the reader can see it */
  n = ringbuf_count(r);
  if (n > r->high_water) r->high_water = n;
}

int ringbuf_push(ringbuf_t *r, const log_rec_t *rec)
{
  log_rec_t *slot = ringbuf_slot(r);

  if (!slot) return 0;                /* full */
  *slot = *rec;
  ringbuf_commit(r);
  return 1;
}

const log_rec_t *ringbuf_peek(const ringbuf_t *r)
{
  uint32_t tail = r->tail;

  return (tail == r->head) ? 0 : &r->buf[tail];   /* empty: nothing to see */
}

void ringbuf_drop(ringbuf_t *r)
{
  uint32_t tail = r->tail;

  if (tail == r->head) return;
  RB_BARRIER();                       /* finished reading the slot before handing it back */
  r->tail = next_index(r, tail);
}

int ringbuf_pop(ringbuf_t *r, log_rec_t *rec)
{
  const log_rec_t *oldest = ringbuf_peek(r);

  if (!oldest) return 0;              /* empty */
  *rec = *oldest;
  ringbuf_drop(r);
  return 1;
}
