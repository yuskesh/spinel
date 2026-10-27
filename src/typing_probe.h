#ifndef SP_TYPING_PROBE_H
#define SP_TYPING_PROBE_H
#include "compiler.h"
extern int sp_typing_probe_enabled;
extern int sp_typing_probe_child;
extern int sp_typing_probe_fd;
extern char *sp_typing_probe_before;
extern char sp_typing_probe_before_hash[65];
extern const char *sp_typing_probe_source;
extern char *sp_typing_probe_stderr;
int sp_typing_probe_prepare(void);
#ifdef SPINEL_CODEGEN_INTERNAL_H
void sp_typing_probe_metadata(Buf *, Compiler *);
#endif
#endif
