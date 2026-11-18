/* jsonl.c - JSON Lines formatter and 512-byte block packer. See jsonl.h. */
#include "jsonl.h"
#include <string.h>

static const char HEX[] = "0123456789ABCDEF";

static char *put_str(char *p, const char *s)
{
  while (*s) *p++ = *s++;
  return p;
}

static char *put_u32(char *p, uint32_t v)
{
  char tmp[10];
  int n = 0;

  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) *p++ = tmp[--n];
  return p;
}

/* 64-bit divides are slow on a Cortex-M4, so split once and print 32-bit halves. */
static char *put_u64(char *p, uint64_t v)
{
  uint32_t group[2], g;
  char tmp[9];
  int groups = 0, n;

  /* Peel off 9-digit groups until the rest fits in 32 bits (at most twice). */
  while (v > 0xFFFFFFFFu) {
    group[groups++] = (uint32_t)(v % 1000000000u);
    v /= 1000000000u;
  }
  p = put_u32(p, (uint32_t)v);
  while (groups) {
    g = group[--groups];
    for (n = 0; n < 9; n++) { tmp[n] = (char)('0' + g % 10); g /= 10; }   /* groups keep their zeros */
    while (n) *p++ = tmp[--n];
  }
  return p;
}

/* Text that needs no escaping is the caller's job; this drops anything that would break JSON. */
static char *put_safe(char *p, const char *s, size_t max)
{
  while (*s && max--) {
    char c = *s++;
    if (c >= 0x20 && c != '"' && c != '\\' && c < 0x7F) *p++ = c;
  }
  return p;
}

static const char *const ERR_STATE[] = { "ok", "warn", "passive", "busoff" };
static const char *const SYS_EVENT[LOGREC_EV_COUNT] = {
  "start", "card_in", "card_out", "powerfail", "recover", "fault", "busoff_restart"
};

size_t jsonl_format(char *out, uint32_t boot, uint32_t n, uint64_t t_us, const log_rec_t *r)
{
  char *p = out;
  uint8_t kind = r->type & LOGREC_KIND_MASK;
  uint8_t i, dlc;

  p = put_str(p, "{\"b\":");   p = put_u32(p, boot);
  p = put_str(p, ",\"n\":");   p = put_u32(p, n);
  p = put_str(p, ",\"t\":");   p = put_u64(p, t_us);
  p = put_str(p, ",\"k\":\"");

  switch (kind) {
  case LOGREC_RX:
  case LOGREC_TX:
    p = put_str(p, kind == LOGREC_RX ? "rx" : "tx");
    p = put_str(p, "\",\"id\":\"0x");
    *p++ = HEX[(r->id >> 8) & 0x7];
    *p++ = HEX[(r->id >> 4) & 0xF];
    *p++ = HEX[r->id & 0xF];
    p = put_str(p, "\",\"d\":\"");
    dlc = (r->dlc > 8) ? 8 : r->dlc;
    if (!(r->type & LOGREC_RTR)) {
      for (i = 0; i < dlc; i++) {
        *p++ = HEX[r->data[i] >> 4];
        *p++ = HEX[r->data[i] & 0xF];
      }
    }
    *p++ = '"';
    if (r->type & LOGREC_RTR) p = put_str(p, ",\"r\":1");
    break;
  case LOGREC_TIME:
    p = put_str(p, "time\",\"rtc\":");   p = put_u32(p, logrec_u32(r, 0));
    break;
  case LOGREC_ERR:
    p = put_str(p, "err\",\"tec\":");    p = put_u32(p, r->data[0]);
    p = put_str(p, ",\"rec\":");         p = put_u32(p, r->data[1]);
    p = put_str(p, ",\"lec\":");         p = put_u32(p, r->data[2]);
    p = put_str(p, ",\"st\":\"");        p = put_str(p, ERR_STATE[r->data[3] & 3]);
    *p++ = '"';
    break;
  case LOGREC_DROP:
    p = put_str(p, "drop\",\"cnt\":");   p = put_u32(p, logrec_u32(r, 0));
    break;
  case LOGREC_SYS:
    p = put_str(p, "sys\",\"ev\":\"");
    p = put_str(p, r->data[0] < LOGREC_EV_COUNT ? SYS_EVENT[r->data[0]] : "unknown");
    *p++ = '"';
    break;
  case LOGREC_TSET:
    p = put_str(p, "tset\",\"old\":");   p = put_u32(p, logrec_u32(r, 0));
    p = put_str(p, ",\"new\":");         p = put_u32(p, logrec_u32(r, 4));
    break;
  default:
    p = put_str(p, "unknown\"");
    break;
  }
  *p++ = '}';
  *p++ = '\n';
  return (size_t)(p - out);
}

size_t jsonl_format_header(char *out, size_t cap, const jsonl_hdr_t *h)
{
  char *p = out;

  if (cap < 256) return 0;    /* fixed text + bounded fields below always fit in 256 */
  p = put_str(p, "{\"k\":\"hdr\",\"v\":");      p = put_u32(p, JSONL_VERSION);
  p = put_str(p, ",\"fw\":\"");                 p = put_safe(p, h->fw, 40);
  p = put_str(p, "\",\"board\":\"");            p = put_safe(p, h->board, 16);
  p = put_str(p, "\",\"b\":");                  p = put_u32(p, h->boot);
  p = put_str(p, ",\"reset\":\"");              p = put_safe(p, h->reset, 16);
  p = put_str(p, "\",\"rtc\":");                p = put_u32(p, h->rtc);
  p = put_str(p, ",\"rtc_ok\":");               p = put_str(p, h->rtc_ok ? "true" : "false");
  p = put_str(p, ",\"bitrate\":");              p = put_u32(p, h->bitrate);
  *p++ = '}';
  *p++ = '\n';
  return (size_t)(p - out);
}

void jsonl_stage_init(jsonl_stage_t *s, char *buf, size_t cap)
{
  s->buf = buf;
  s->cap = cap - cap % JSONL_BLOCK;
  s->len = 0;
}

void jsonl_stage_pad(jsonl_stage_t *s)
{
  size_t used = s->len % JSONL_BLOCK;

  if (used == 0) return;
  memset(s->buf + s->len, ' ', JSONL_BLOCK - used);
  s->len += JSONL_BLOCK - used;
  s->buf[s->len - 1] = '\n';
}

int jsonl_stage_add(jsonl_stage_t *s, const char *line, size_t n)
{
  size_t room = JSONL_BLOCK - s->len % JSONL_BLOCK;
  size_t start = (n > room) ? s->len + room : s->len;   /* skip to the next block if it won't fit */

  if (n > JSONL_BLOCK || start + n > s->cap) return 0;
  if (n > room) jsonl_stage_pad(s);
  memcpy(s->buf + s->len, line, n);
  s->len += n;
  return 1;
}
