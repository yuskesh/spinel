/* sp_io.c -- File / IO handle ops in libspinel_rt.a.
 *
 * The allocation-free handle ops (open / pipe / fdopen / write / close /
 * closed? / puts / print / flush / eof?); the string-returning readers
 * (gets / read / read_n / path) stay inline in sp_runtime.h.
 *
 * Self-contained: includes sp_io.h (the sp_File layout) + sp_gc.h
 * (sp_mark_string), not sp_runtime.h. */
#include "sp_io.h"
#include "sp_gc.h"   /* sp_mark_string */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* pipe */
#include <sys/stat.h> /* stat() for the File predicates */

/* Provided by the generated TU / libspinel_rt.a. */
extern void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
extern SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);

static void sp_File_fin(void *p) { sp_File *f = (sp_File *)p; if (f->fp) { fclose(f->fp); f->fp = NULL; } }
static void sp_File_scan(void *p) { sp_File *f = (sp_File *)p; if (f->path) sp_mark_string(f->path); if (f->mode) sp_mark_string(f->mode); }

sp_File *sp_File_open(const char *path, const char *mode) {
  sp_File *f = (sp_File *)sp_gc_alloc(sizeof(sp_File), sp_File_fin, sp_File_scan);
  f->fp = fopen(path ? path : "", mode ? mode : "r");
  if (!f->fp) { sp_raise_cls("Errno::ENOENT", "No such file or directory"); return NULL; }
  f->path = path;
  f->mode = mode;
  return f;
}

/* Returns 0 on success, -1 on error. */
int sp_io_make_pipe(int fds[2]) {
  return pipe(fds);
}

/* IO.pipe end: wrap a raw pipe fd in a GC-managed sp_File so the
   sp_File_* I/O ops work on it. Same finalizer/scan as sp_File_open. */
sp_File *sp_io_fdopen(int fd, const char *mode) {
  sp_File *f = (sp_File *)sp_gc_alloc(sizeof(sp_File), sp_File_fin, sp_File_scan);
  f->fp = fdopen(fd, mode ? mode : "r");
  if (!f->fp) { sp_raise_cls("IOError", "fdopen failed"); return NULL; }
  f->path = NULL;
  f->mode = mode;
  return f;
}

mrb_int sp_File_write(sp_File *f, const char *s) {
  if (!f || !f->fp || !s) return 0;
  size_t n = strlen(s);
  return (mrb_int)fwrite(s, 1, n, f->fp);
}

sp_File *sp_io_stdout(void) {
  static sp_File f = { NULL, "<STDOUT>", "w" };
  if (!f.fp) f.fp = stdout;
  return &f;
}

sp_File *sp_io_stderr(void) {
  static sp_File f = { NULL, "<STDERR>", "w" };
  if (!f.fp) f.fp = stderr;
  return &f;
}

mrb_int sp_File_close(sp_File *f) {
  /* never fclose the shared stdout/stderr handles (sp_io_stdout/sp_io_stderr):
     closing the process's standard streams would corrupt the singleton and any
     later write through it. Closing them is a no-op. */
  if (f && f->fp && f->fp != stdout && f->fp != stderr) { fclose(f->fp); f->fp = NULL; }
  return 0;
}

mrb_bool sp_File_closed_p(sp_File *f) {
  return !f || !f->fp;
}

void sp_File_puts(sp_File *f, const char *s) {
  if (!f || !f->fp || !s) return;
  size_t n = strlen(s);
  fputs(s, f->fp);
  if (n == 0 || s[n - 1] != '\n') fputc('\n', f->fp);
}

void sp_File_print(sp_File *f, const char *s) {
  if (!f || !f->fp || !s) return;
  fputs(s, f->fp);
}

mrb_int sp_File_flush(sp_File *f) {
  if (f && f->fp) fflush(f->fp);
  return 0;
}

mrb_bool sp_File_eof_p(sp_File *f) {
  if (!f || !f->fp) return TRUE;
  int c = fgetc(f->fp);
  if (c == EOF) return TRUE;
  ungetc(c, f->fp);
  return FALSE;
}

/* ---- File metadata predicates ----
   libc / WinAPI only, no spinel-string allocation and no shared mutable
   state, so they live here rather than inline in sp_runtime.h. */
mrb_bool sp_file_directory(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

mrb_bool sp_file_file(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

mrb_bool sp_file_exist(const char *path) { FILE *f = fopen(path, "r"); if (f) { fclose(f); return TRUE; } return FALSE; }
void sp_file_delete(const char *path) { remove(path); }
