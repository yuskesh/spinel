/* sp_time.c -- libc-backed Time implementations.
 *
 * Sibling to sp_bigint.c / sp_crypto.c. The libc value ops (construct,
 * accessors, shifts) carry no runtime dependency; the formatters
 * (strftime / iso8601 / zone / inspect) return GC-heap strings directly
 * via sp_alloc.h, so the generated TU no longer needs buffer-copying
 * trampolines for them.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sp_time.h"
#include "sp_alloc.h"   /* sp_str_dup_external / sp_str_empty for the GC formatters */

sp_Time sp_time_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (sp_Time){ ts.tv_sec, (int32_t)ts.tv_nsec, 0 };
}

sp_Time sp_time_at_int(int64_t sec) {
  return (sp_Time){ sec, 0, 0 };
}

/* POSIX convention: keep tv_nsec in [0, 1e9). For negative epoch with
   a non-integer fractional part, decrement tv_sec and roll the fraction
   into the positive nsec range — so Time.at(-0.5).to_i returns -1, not 0. */
sp_Time sp_time_at_float(double epoch) {
  int64_t sec = (int64_t)epoch;
  double frac = epoch - (double)sec;
  if (frac < 0.0) {
    sec -= 1;
    frac += 1.0;
  }
  return (sp_Time){ sec, (int32_t)(frac * 1e9), 0 };
}

/* Time.new(y[,mo[,d[,h[,mi[,s]]]]]) — local construction. mktime
   interprets the broken-down value in the host local zone and resolves
   DST itself (tm_isdst=-1). The fixed-offset 7-arg form is a separate
   issue. */
sp_Time sp_time_new(int64_t y, int64_t mo, int64_t d,
                    int64_t h, int64_t mi, int64_t s) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = (int)y - 1900;
  tm.tm_mon  = (int)mo - 1;
  tm.tm_mday = (int)d;
  tm.tm_hour = (int)h;
  tm.tm_min  = (int)mi;
  tm.tm_sec  = (int)s;
  tm.tm_isdst = -1;
  time_t e = mktime(&tm);
  return (sp_Time){ (int64_t)e, 0, 0 };
}

/* Time.utc(y, m, d, h, mi, s) — UTC construction. Avoid timegm
   (not portable; absent on MSVCRT). Compute the epoch via Howard
   Hinnant's days_from_civil + a manual hour/minute/second add. */
sp_Time sp_time_new_utc(int64_t y, int64_t mo, int64_t d,
                        int64_t h, int64_t mi, int64_t s) {
  int64_t yy = y - (mo <= 2 ? 1 : 0);
  int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  int64_t yoe = yy - era * 400;
  int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  int64_t e = days * 86400 + h * 3600 + mi * 60 + s;
  return (sp_Time){ e, 0, 1 };
}

sp_Time sp_time_utc(sp_Time t) {
  t.is_utc = 1;
  return t;
}

sp_Time sp_time_localtime(sp_Time t) {
  t.is_utc = 0;
  return t;
}

/* is_utc selects gmtime vs localtime, off is UTC offset in seconds,
   zbuf is the timezone abbreviation (8 bytes). mktime(gmtime(s))-s
   is the portable offset technique (MSVCRT's %z emits the timezone
   name, not ±HHMM). */
void sp_time_vtm(sp_Time t, struct tm *bd, int32_t *off, char *zbuf) {
  time_t s = (time_t)t.tv_sec;
  if (t.is_utc) {
    struct tm *g = gmtime(&s);
    if (g) { *bd = *g; }
else { memset(bd, 0, sizeof(*bd)); }
    if (off) *off = 0;
    if (zbuf) { zbuf[0]='U'; zbuf[1]='T'; zbuf[2]='C'; zbuf[3]=0; }
  }
else {
    struct tm *l = localtime(&s);
    if (l) { *bd = *l; }
else { memset(bd, 0, sizeof(*bd)); }
    if (off) {
      struct tm gm = *gmtime(&s);
      gm.tm_isdst = -1;
      *off = (int32_t)(s - (time_t)mktime(&gm));
    }
    if (zbuf) {
      if (strftime(zbuf, 8, "%Z", bd) == 0) zbuf[0] = 0;
    }
  }
}

int64_t sp_time_year(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)(b.tm_year+1900);}
int64_t sp_time_mon(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)(b.tm_mon+1);}
int64_t sp_time_mday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_mday;}
int64_t sp_time_hour(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_hour;}
int64_t sp_time_min(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_min;}
int64_t sp_time_sec(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_sec;}
int64_t sp_time_wday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_wday;}
int64_t sp_time_yday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)(b.tm_yday+1);}
int64_t sp_time_isdst(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)(b.tm_isdst>0?1:0);}
int64_t sp_time_utc_offset(sp_Time t){int32_t o;struct tm b;sp_time_vtm(t,&b,&o,NULL);return (int64_t)o;}

/* Time + Numeric / Time - Numeric. secs may be fractional, so split
   into whole seconds plus a sub-second carry and keep tv_nsec
   normalized to [0,1e9). is_utc is inherited from the receiver. */
sp_Time sp_time_add(sp_Time t, double secs) {
  int64_t whole = (int64_t)secs;
  double frac = secs - (double)whole;
  int64_t ns = (int64_t)t.tv_nsec + (int64_t)(frac * 1e9);
  int64_t carry = ns / 1000000000;
  ns -= carry * 1000000000;
  if (ns < 0) { ns += 1000000000; carry -= 1; }
  return (sp_Time){ t.tv_sec + whole + carry, (int32_t)ns, t.is_utc };
}

/* strftime returns 0 -- never overruns the buffer -- when the formatted
   result would exceed it, which we surface as "". The 4 KB buffer covers
   any realistic format (CRuby's built-ins are ~25 bytes; this leaves room
   for long literal text or wide fields). A pathological field width
   (`"%1000000000F"`, which CRuby rejects with ERANGE) does not fit and
   yields "" -- a graceful empty string rather than a crash. */
const char *sp_time_strftime(sp_Time t, const char *fmt) {
  char buf[4096];
  time_t s = (time_t)t.tv_sec;
  struct tm *lt = t.is_utc ? gmtime(&s) : localtime(&s);
  if (lt == NULL) return sp_str_empty;
  size_t n = strftime(buf, sizeof(buf), fmt, lt);
  if (n == 0) return sp_str_empty;
  buf[n] = 0;
  return sp_str_dup_external(buf);
}

/* RFC 3339 / iso8601. Format date+time prefix with strftime, then
   compute the UTC offset manually via mktime(gmtime(s)) - s (MSVCRT
   %z renders the timezone name, so we do it ourselves). */
const char *sp_time_iso8601(sp_Time t) {
  char buf[64];
  size_t cap = sizeof(buf);
  time_t s = (time_t)t.tv_sec;
  if (t.is_utc) {
    struct tm *gt = gmtime(&s);
    if (gt == NULL) return sp_str_empty;
    size_t n = strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", gt);
    if (n == 0) return sp_str_empty;
    return sp_str_dup_external(buf);
  }
  struct tm *lt = localtime(&s);
  if (lt == NULL) return sp_str_empty;
  size_t n = strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", lt);
  if (n == 0) return sp_str_empty;
  if (n + 6 < cap) {
    struct tm gm = *gmtime(&s);
    gm.tm_isdst = -1;
    time_t gm_as_if_local = mktime(&gm);
    long offset_sec = (long)(s - gm_as_if_local);
    char sign = offset_sec >= 0 ? '+' : '-';
    long abs_off = offset_sec < 0 ? -offset_sec : offset_sec;
    int oh = (int)(abs_off / 3600);
    int om = (int)((abs_off / 60) % 60);
    buf[n++] = sign;
    buf[n++] = (char)('0' + (oh / 10));
    buf[n++] = (char)('0' + (oh % 10));
    buf[n++] = ':';
    buf[n++] = (char)('0' + (om / 10));
    buf[n++] = (char)('0' + (om % 10));
    buf[n] = 0;
  }
  return sp_str_dup_external(buf);
}

const char *sp_time_zone(sp_Time t) {
  char buf[8];
  struct tm b;
  sp_time_vtm(t, &b, NULL, buf);
  return sp_str_dup_external(buf);
}

/* Scalar Time inspect. CRuby form: local "YYYY-MM-DD HH:MM:SS +0900",
   UTC "YYYY-MM-DD HH:MM:SS UTC". The poly-box path keeps its own
   sp_Time_inspect; this value-taking variant is for the scalar
   p/puts/to_s codegen path. */
const char *sp_time_inspect_v(sp_Time t) {
  char buf[40];
  size_t cap = sizeof(buf);
  struct tm b;
  int32_t off;
  sp_time_vtm(t, &b, &off, NULL);
  size_t n = strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &b);
  if (n == 0) {
    snprintf(buf, cap, "Time(%lld)", (long long)t.tv_sec);
    return sp_str_dup_external(buf);
  }
  if (n + 8 < cap) {
    if (t.is_utc) {
      buf[n++]=' '; buf[n++]='U'; buf[n++]='T'; buf[n++]='C'; buf[n]=0;
    }
else {
      char sign = off >= 0 ? '+' : '-';
      long a = off < 0 ? -(long)off : (long)off;
      int oh = (int)(a / 3600);
      int om = (int)((a / 60) % 60);
      buf[n++]=' '; buf[n++]=sign;
      buf[n++]=(char)('0'+oh/10); buf[n++]=(char)('0'+oh%10);
      buf[n++]=(char)('0'+om/10); buf[n++]=(char)('0'+om%10);
      buf[n]=0;
    }
  }
  return sp_str_dup_external(buf);
}

/* ---- comparison + shifts (moved from sp_runtime.h; cold) ---- */
int sp_time_cmp(sp_Time a, sp_Time b) {
  if (a.tv_sec < b.tv_sec) return -1;
  if (a.tv_sec > b.tv_sec) return 1;
  if (a.tv_nsec < b.tv_nsec) return -1;
  if (a.tv_nsec > b.tv_nsec) return 1;
  return 0;
}
sp_Time sp_time_add_f(sp_Time t, double secs) {
  long long ns = (long long)(secs * 1000000000.0);
  long long total_ns = ((long long)t.tv_sec * 1000000000LL) + t.tv_nsec + ns;
  sp_Time r;
  r.tv_sec = (time_t)(total_ns / 1000000000LL);
  r.tv_nsec = (int32_t)(total_ns % 1000000000LL);
  if (r.tv_nsec < 0) { r.tv_sec--; r.tv_nsec += 1000000000; }
  r.is_utc = t.is_utc;
  return r;
}
sp_Time sp_time_add_i(sp_Time t, int64_t secs) {
  sp_Time r;
  r.tv_sec = t.tv_sec + (time_t)secs;
  r.tv_nsec = t.tv_nsec;
  r.is_utc = t.is_utc;
  return r;
}
sp_Time sp_time_sub_i(sp_Time t, int64_t secs) {
  sp_Time r;
  r.tv_sec = t.tv_sec - (time_t)secs;
  r.tv_nsec = t.tv_nsec;
  r.is_utc = t.is_utc;
  return r;
}
double sp_time_sub_t(sp_Time a, sp_Time b) {
  return (double)(a.tv_sec - b.tv_sec) + ((double)(a.tv_nsec - b.tv_nsec) / 1e9);
}
