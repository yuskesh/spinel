/* sp_io.c -- File / IO handle ops in libspinel_rt.a.
 *
 * The allocation-free handle ops (open / pipe / fdopen / write / close /
 * closed? / puts / print / flush / eof?). The string-returning readers
 * (gets / read / read_n / path) stay inline in sp_runtime.h since they
 * allocate via the hot static sp_str_alloc, whose per-TU sp_str_heap
 * can't be shared across translation units.
 *
 * Self-contained: includes sp_io.h (the sp_File layout) + sp_gc.h
 * (sp_mark_string), but not sp_runtime.h, so it avoids the mruby_shim.h
 * mrb_bool conflict (same convention as sp_gc.c / sp_fiber.c). */
#include "sp_io.h"
#include "sp_gc.h"   /* sp_mark_string */
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>       /* _pipe */
#include <fcntl.h>    /* _O_BINARY */
#include <windows.h>  /* GetFileAttributesW for the File predicates */
#else
#include <unistd.h>   /* pipe */
#include <sys/stat.h> /* stat() for the File predicates */
#endif

/* Provided by the generated TU / libspinel_rt.a. */
extern void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
extern void sp_raise_cls(const char *cls, const char *msg);

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

/* pipe(2) portability: MinGW exposes _pipe(fds, size, mode) via <io.h>,
   not the POSIX pipe(int[2]). Returns 0 on success, -1 on error. */
int sp_io_make_pipe(int fds[2]) {
#ifdef _WIN32
  return _pipe(fds, 65536, _O_BINARY);
#else
  return pipe(fds);
#endif
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

mrb_int sp_File_close(sp_File *f) {
  if (f && f->fp) { fclose(f->fp); f->fp = NULL; }
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
#ifdef _WIN32
static DWORD sp_file_attributes(const char *path) {
  if (!path) return INVALID_FILE_ATTRIBUTES;
  int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (len <= 0) return INVALID_FILE_ATTRIBUTES;
  wchar_t *wpath = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)len);
  if (!wpath) return INVALID_FILE_ATTRIBUTES;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, len) <= 0) {
    free(wpath);
    return INVALID_FILE_ATTRIBUTES;
  }
  DWORD attrs = GetFileAttributesW(wpath);
  free(wpath);
  return attrs;
}

mrb_bool sp_file_directory(const char *path) {
  DWORD attrs = sp_file_attributes(path);
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

mrb_bool sp_file_file(const char *path) {
  DWORD attrs = sp_file_attributes(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
#else
mrb_bool sp_file_directory(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

mrb_bool sp_file_file(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
#endif

mrb_bool sp_file_exist(const char *path) { FILE *f = fopen(path, "r"); if (f) { fclose(f); return TRUE; } return FALSE; }
void sp_file_delete(const char *path) { remove(path); }
