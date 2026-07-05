/* sp_base64.c -- Base64 for the `base64` spin package (Path B carried C),
   linked on demand when `require "base64"` appears.

   CRuby-compatible surface: encode64 (RFC 2045: "+/" alphabet, padded, a
   newline every 60 output chars and after the tail), strict_encode64
   (RFC 4648: padded, no newlines), urlsafe_encode64 ("-_" alphabet, padded
   like CRuby's default padding: true), and the matching decoders. Binary
   safe on both sides: input length comes from the string header
   (sp_str_byte_len), and decoded output is length-set so embedded NULs
   survive. Results live on the GC string heap. */
#include "spinel/runtime.h"
#include <string.h>

static const char B64_STD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64_URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* encode `n` bytes of `src` with `alpha`; a positive `wrap` inserts a newline
   every `wrap` output chars and after the tail (CRuby encode64 wraps at 60). */
static const char *b64_enc(const char *src, size_t n, const char *alpha, int wrap) {
  size_t groups = (n + 2) / 3;
  size_t outlen = groups * 4;
  size_t nl = wrap > 0 ? (outlen + (size_t)wrap - 1) / (size_t)wrap : 0;
  char *r = sp_str_alloc_raw(outlen + nl + 1);
  size_t o = 0, col = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned char)src[i] << 16;
    if (i + 1 < n) v |= (unsigned char)src[i + 1] << 8;
    if (i + 2 < n) v |= (unsigned char)src[i + 2];
    char q[4];
    q[0] = alpha[(v >> 18) & 63];
    q[1] = alpha[(v >> 12) & 63];
    q[2] = i + 1 < n ? alpha[(v >> 6) & 63] : '=';
    q[3] = i + 2 < n ? alpha[v & 63] : '=';
    for (int k = 0; k < 4; k++) {
      r[o++] = q[k];
      if (wrap > 0 && ++col == (size_t)wrap) { r[o++] = '\n'; col = 0; }
    }
  }
  if (wrap > 0 && col > 0) r[o++] = '\n';
  r[o] = '\0';
  sp_str_set_len(r, o);
  return r;
}

/* decode, skipping whitespace; '-_' and '+/' both accepted so one decoder
   serves the standard and urlsafe forms (CRuby's decode64 is lax the same
   way for whitespace; alphabets stay distinct per entry point below). */
static int b64_val(char c, const char *alpha) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == alpha[62]) return 62;
  if (c == alpha[63]) return 63;
  return -1;
}

static const char *b64_dec(const char *src, size_t n, const char *alpha) {
  char *r = sp_str_alloc_raw(n / 4 * 3 + 4);
  size_t o = 0;
  unsigned acc = 0;
  int nb = 0;
  for (size_t i = 0; i < n; i++) {
    char c = src[i];
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '=') continue;
    int v = b64_val(c, alpha);
    if (v < 0) continue;   /* CRuby decode64 ignores out-of-alphabet bytes */
    acc = (acc << 6) | (unsigned)v;
    if (++nb == 4) { r[o++] = (char)(acc >> 16); r[o++] = (char)(acc >> 8); r[o++] = (char)acc; acc = 0; nb = 0; }
  }
  if (nb == 3) { acc <<= 6; r[o++] = (char)(acc >> 16); r[o++] = (char)(acc >> 8); }
  else if (nb == 2) { acc <<= 12; r[o++] = (char)(acc >> 16); }
  r[o] = '\0';
  sp_str_set_len(r, o);
  return r;
}

const char *sp_base64_encode64(const char *s)        { return b64_enc(s, sp_str_byte_len(s), B64_STD, 60); }
const char *sp_base64_strict_encode64(const char *s) { return b64_enc(s, sp_str_byte_len(s), B64_STD, 0); }
const char *sp_base64_urlsafe_encode64(const char *s){ return b64_enc(s, sp_str_byte_len(s), B64_URL, 0); }
const char *sp_base64_decode64(const char *s)        { return b64_dec(s, sp_str_byte_len(s), B64_STD); }
const char *sp_base64_strict_decode64(const char *s) { return b64_dec(s, sp_str_byte_len(s), B64_STD); }
const char *sp_base64_urlsafe_decode64(const char *s){ return b64_dec(s, sp_str_byte_len(s), B64_URL); }
