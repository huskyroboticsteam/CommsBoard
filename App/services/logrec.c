/* logrec.c - packing helpers for the 16-byte binary record. See logrec.h. */
#include "logrec.h"
#include <string.h>

_Static_assert(sizeof(log_rec_t) == 16, "log_rec_t must be 16 bytes");

static void put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void start(log_rec_t *r, uint8_t kind, uint32_t t_us)
{
  memset(r, 0, sizeof *r);
  r->t_us = t_us;
  r->type = kind;
}

void logrec_frame(log_rec_t *r, uint8_t kind, uint32_t t_us, uint16_t id,
                  uint8_t dlc, int rtr, const uint8_t *data)
{
  start(r, kind, t_us);
  if (dlc > 8) dlc = 8;
  r->id = id & 0x7FF;
  r->dlc = dlc;
  if (rtr) r->type |= LOGREC_RTR;
  else memcpy(r->data, data, dlc);     /* a remote frame carries no data */
}

void logrec_time(log_rec_t *r, uint32_t t_us, uint32_t rtc_unix)
{
  start(r, LOGREC_TIME, t_us);
  put_u32(r->data, rtc_unix);
}

void logrec_err(log_rec_t *r, uint32_t t_us, uint8_t tec, uint8_t rec, uint8_t lec, uint8_t state)
{
  start(r, LOGREC_ERR, t_us);
  r->data[0] = tec; r->data[1] = rec; r->data[2] = lec; r->data[3] = state;
}

void logrec_drop(log_rec_t *r, uint32_t t_us, uint32_t count)
{
  start(r, LOGREC_DROP, t_us);
  put_u32(r->data, count);
}

void logrec_sys(log_rec_t *r, uint32_t t_us, uint8_t event)
{
  start(r, LOGREC_SYS, t_us);
  r->data[0] = event;
}

void logrec_tset(log_rec_t *r, uint32_t t_us, uint32_t old_unix, uint32_t new_unix)
{
  start(r, LOGREC_TSET, t_us);
  put_u32(r->data, old_unix);
  put_u32(r->data + 4, new_unix);
}

uint32_t logrec_u32(const log_rec_t *r, int offset)
{
  const uint8_t *p = r->data + offset;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
