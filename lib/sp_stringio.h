#ifndef SP_STRINGIO_H
#define SP_STRINGIO_H
/* sp_stringio.h -- StringIO, split out of sp_runtime.h into libspinel_rt.a.

   A StringIO wraps a plain malloc'd char buffer (not the GC string heap and not
   sp_String), so it is fully self-contained: only the shared string allocator
   (sp_alloc.h, for the GC strings its readers hand back) and sp_raise_cls. The
   struct is declared here so the generated TU can hold `sp_StringIO *` values;
   the methods are compiled once in the archive. */
#include "sp_types.h"   /* mrb_bool, int64_t */

typedef struct { char *buf; int64_t len; int64_t cap; int64_t pos; int64_t lineno; int closed; } sp_StringIO;

sp_StringIO *sp_StringIO_new(void);
sp_StringIO *sp_StringIO_new_s(const char *init);
sp_StringIO *sp_StringIO_new_sm(const char *init, const char *mode);
const char *sp_StringIO_string(sp_StringIO *s);
int64_t sp_StringIO_pos(sp_StringIO *s);
int64_t sp_StringIO_size(sp_StringIO *s);
int64_t sp_StringIO_write(sp_StringIO *s, const char *str);
int64_t sp_StringIO_puts(sp_StringIO *s, const char *str);
int64_t sp_StringIO_puts_empty(sp_StringIO *s);
int64_t sp_StringIO_print(sp_StringIO *s, const char *str);
int64_t sp_StringIO_putc(sp_StringIO *s, int64_t ch);
const char *sp_StringIO_read(sp_StringIO *s);
const char *sp_StringIO_read_n(sp_StringIO *s, int64_t n);
const char *sp_StringIO_gets(sp_StringIO *s);
const char *sp_StringIO_getc(sp_StringIO *s);
int64_t sp_StringIO_getbyte(sp_StringIO *s);
int64_t sp_StringIO_rewind(sp_StringIO *s);
int64_t sp_StringIO_seek(sp_StringIO *s, int64_t off);
int64_t sp_StringIO_tell(sp_StringIO *s);
mrb_bool sp_StringIO_eof_p(sp_StringIO *s);
int64_t sp_StringIO_truncate(sp_StringIO *s, int64_t l);
int64_t sp_StringIO_close(sp_StringIO *s);
mrb_bool sp_StringIO_closed_p(sp_StringIO *s);
sp_StringIO *sp_StringIO_flush(sp_StringIO *s);
mrb_bool sp_StringIO_sync(sp_StringIO *s);
mrb_bool sp_StringIO_isatty(sp_StringIO *s);
#endif /* SP_STRINGIO_H */
