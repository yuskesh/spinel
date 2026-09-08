/* sp_alloc_report_signal.c -- the operating-system half of SPINEL_ALLOC_REPORT's
   on-demand dump: a signal (SIGUSR1 by default, SPINEL_ALLOC_REPORT_SIGNAL to
   move it) asks for the report of a program that is still running.

   The report itself -- the counters, the dump, the pending flag and the poll
   on the counting path -- is portable and lives in sp_alloc.c. What needs
   sigaction, a pipe and (under SP_THREADS) a thread parked on it lives here,
   so that a target without signals (wasm32-wasi) still builds the allocator
   and simply has no trigger. Like sp_system.c and sp_process.c this TU is
   libc + OS API only; nothing in the hot path reaches into it.

   The handler cannot write the report: fopen, malloc and backtrace_symbols
   are none of them async-signal-safe. What it can do is `write` one byte to a
   pipe, which is, and a thread parked on the other end does the writing. That
   thread is why the signal works on an IDLE server -- polling the flag from
   the counting path alone made a server with no traffic wait for the next
   request instead of answering when asked.

   Without threads there is no such thread to park, and a single-threaded
   program that is idle is inside a syscall with nothing else to run: there
   the flag and the counting path are the only mechanism available, and the
   dump lands on the next allocation. The handler picks whichever is armed. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp (that signal named rather than numbered) */
#include <unistd.h>    /* write/read: the handler's only safe way to say "dump" */
#include <fcntl.h>     /* the wake pipe's O_NONBLOCK and FD_CLOEXEC */
#include <errno.h>     /* EINTR on the reader's park */
#ifdef SP_THREADS
#include <pthread.h>   /* the thread that does the writing */
#endif
#include "sp_alloc.h"

static int sp_alloc_report_pipe[2] = { -1, -1 };

static void sp_alloc_report_handler(int sig) {
  (void)sig;
  if (sp_alloc_report_pipe[1] >= 0) {
    char b = 1;
    /* Non-blocking, so a full pipe (or a fork'd child, which inherited the
       handler but not the thread that reads it) degrades to the flag rather
       than blocking inside a signal handler. */
    if (write(sp_alloc_report_pipe[1], &b, 1) == 1) return;
  }
  sp_alloc_report_request();
}

/* A number (so real-time signals work) or a name with or without the SIG
   prefix. SIGUSR1 by default because a program that wants it for itself can
   move this one, and the handler is installed only while the report is on.
   Kept as a small copy of sp_resolve_preempt_signal rather than a shared
   helper: the allocator sits below the scheduler and should not reach up into
   it for a dozen lines. */
static int sp_resolve_report_signal(void) {
  const char *e = getenv("SPINEL_ALLOC_REPORT_SIGNAL");
  if (!e || !*e) return SIGUSR1;
  char *end;
  long n = strtol(e, &end, 10);
  if (*end == '\0') { if (n > 0 && n < 65) return (int)n; }
  else {
    const char *name = e;
    if (strncasecmp(name, "SIG", 3) == 0) name += 3;
    static const struct { const char *n; int s; } tab[] = {
      { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "URG", SIGURG },
      { "IO", SIGIO }, { "WINCH", SIGWINCH },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
      if (strcasecmp(name, tab[i].n) == 0) return tab[i].s;
  }
  fprintf(stderr, "spinel: ignoring unrecognized SPINEL_ALLOC_REPORT_SIGNAL=%s;"
                  " using SIGUSR1\n", e);
  return SIGUSR1;
}

#ifdef SP_THREADS
/* Parked on the pipe for the life of the process. Every dump rewrites the
   whole cumulative table, the same one atexit writes, so a window is two
   dumps subtracted rather than a mode of its own -- which is also what
   excludes a server's boot from the profile. Two signals in quick succession
   write it twice; the second overwrites the first, which is harmless. */
static void *sp_alloc_report_reader(void *arg) {
  (void)arg;
  for (;;) {
    char b;
    ssize_t n = read(sp_alloc_report_pipe[0], &b, 1);
    if (n == 0) return NULL;
    if (n < 0) { if (errno == EINTR) continue; return NULL; }
    sp_alloc_report_dump();
  }
}

/* The read end stays blocking, which is how the thread parks for the life of
   the process at no cost; only the WRITE end is non-blocking, because that is
   the one a signal handler touches and it must never block. Both are
   close-on-exec, so an exec'd child does not inherit a pipe nobody reads.
   Failure here is not fatal: the handler falls back to the flag. */
static void sp_alloc_report_start_reader(void) {
  if (pipe(sp_alloc_report_pipe) != 0) { sp_alloc_report_pipe[0] = sp_alloc_report_pipe[1] = -1; return; }
  fcntl(sp_alloc_report_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(sp_alloc_report_pipe[1], F_SETFD, FD_CLOEXEC);
  fcntl(sp_alloc_report_pipe[1], F_SETFL,
        fcntl(sp_alloc_report_pipe[1], F_GETFL, 0) | O_NONBLOCK);
  pthread_t tid;
  if (pthread_create(&tid, NULL, sp_alloc_report_reader, NULL) != 0) {
    close(sp_alloc_report_pipe[0]); close(sp_alloc_report_pipe[1]);
    sp_alloc_report_pipe[0] = sp_alloc_report_pipe[1] = -1;
    return;
  }
  pthread_detach(tid);
}
#endif

/* Called by sp_alloc.c's constructor once it has decided the report is on.
   A plain call rather than a constructor of this TU's own: an archive member
   nobody references is never linked, and a constructor is not a reference. The
   call is also what orders the two halves -- the flag and the dump exist before
   any signal can arrive. */
void sp_alloc_report_arm_trigger(void) {
#ifdef SP_THREADS
  sp_alloc_report_start_reader();
#endif
  /* SA_RESTART: asking for a report must not turn an in-flight read(2) into
     an EINTR the program never expected to handle. */
  { struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = sp_alloc_report_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sp_resolve_report_signal(), &sa, NULL); }
}
