/* sp_io.h -- File / IO handle surface.
 *
 * sp_File is a stdio FILE* plus its (GC-managed) path/mode strings,
 * shared between the generated translation unit and lib/sp_io.c, which
 * holds the allocation-free handle ops. The string-returning readers
 * (sp_File_gets / _read / _read_n / _path) stay inline in sp_runtime.h
 * because they allocate via the hot static sp_str_alloc; moving them
 * would split the per-TU string heap. */
#ifndef SP_IO_H
#define SP_IO_H

#include <stdio.h>
#include "sp_types.h"   /* mrb_int, mrb_bool */

typedef struct { FILE *fp; const char *path; const char *mode; } sp_File;

/* File.open(path, mode) -> GC-managed handle (block form is codegen-only). */
sp_File *sp_File_open(const char *path, const char *mode);
/* pipe(2) wrapper. 0 ok, -1 error. */
int sp_io_make_pipe(int fds[2]);
/* IO.pipe end: wrap a raw pipe fd in a GC-managed sp_File. */
sp_File *sp_io_fdopen(int fd, const char *mode);
mrb_int sp_File_write(sp_File *f, const char *s);
mrb_int sp_File_close(sp_File *f);
mrb_bool sp_File_closed_p(sp_File *f);
void sp_File_puts(sp_File *f, const char *s);
void sp_File_print(sp_File *f, const char *s);
mrb_int sp_File_flush(sp_File *f);
mrb_bool sp_File_eof_p(sp_File *f);

/* STDOUT / STDERR as shared IO handles wrapping the C stdout/stderr streams.
   The handle is a function-local static (stdout/stderr are not constant
   initializers) and is never closed. */
sp_File *sp_io_stdout(void);
sp_File *sp_io_stderr(void);

/* File metadata predicates (libc/WinAPI only; defined in sp_io.c). */
mrb_bool sp_file_directory(const char *path);
mrb_bool sp_file_file(const char *path);
mrb_bool sp_file_exist(const char *path);
void sp_file_delete(const char *path);

#endif
