/* ringbuf.c - single-writer, single-reader ring of log records. See ringbuf.h. */
#include "ringbuf.h"

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

int ringbuf_push(ringbuf_t *r, const log_rec_t *rec)
{
  uint32_t head = r->head;
  uint32_t next = (head + 1 == r->cap) ? 0 : head + 1;
  uint32_t n;

  if (next == r->tail) return 0;      /* full */
  r->buf[head] = *rec;
  r->head = next;                     /* publish only after the record is complete */

  n = ringbuf_count(r);
  if (n > r->high_water) r->high_water = n;
  return 1;
}

int ringbuf_pop(ringbuf_t *r, log_rec_t *rec)
{
  uint32_t tail = r->tail;

  if (tail == r->head) return 0;      /* empty */
  *rec = r->buf[tail];
  r->tail = (tail + 1 == r->cap) ? 0 : tail + 1;
  return 1;
}
