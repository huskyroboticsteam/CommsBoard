/**
  * fmt.h - tiny text formatters used instead of printf/snprintf, which are slow, use a
  * lot of stack and pull malloc into the firmware.
  *
  * Each function writes at p and returns the new end of the text. Nothing is
  * null-terminated and nothing is bounds-checked: the caller sizes the buffer.
  * No HAL here: this compiles and runs in the PC unit tests.
  */
#ifndef FMT_H
#define FMT_H

#include <stdint.h>

static inline char *fmt_str(char *p, const char *s)
{
  while (*s) *p++ = *s++;
  return p;
}

static inline char *fmt_u32(char *p, uint32_t v)    /* decimal, at most 10 characters */
{
  char tmp[10];
  int n = 0;

  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) *p++ = tmp[--n];
  return p;
}

#endif /* FMT_H */
