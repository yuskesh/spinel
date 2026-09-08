/* lib/sp_process.c -- Process.spawn and Process.waitpid2 (CRuby-compatible)
 *
 * Lives in its own TU because the codegen calls sp_process_spawn
 * with pre-resolved positional args (in_fd, out_fd, err_fd, pgroup,
 * rlimit_cpu, rlimit_as, chdir). All opts-hash unpacking happens at
 * compile time in the codegen; the runtime just needs primitive int
 * / int-fd / int-pgroup / int-rlimit values. The cmd is either a
 * String or an Array (boxed as a PolyArray). args is a PolyArray of
 * extra String args appended after cmd.
 *
 * The TU deliberately does NOT include spinel_rt.h (it pulls in only
 * sp_alloc.h and the low-level headers). spinel_rt.h's static-inline
 * family references sp_class_to_s / sp_sym_to_s which are emitted
 * per-program by the codegen and not present in the runtime archive;
 * including spinel_rt.h from here would force those symbols into the
 * link and break the build.
 *
 * Process.spawn(cmd, *args, opts) -> child pid
 * Process.waitpid2(pid) -> [pid, raw_status]
 *
 * opts is a PolyArray of 7 elements in this order:
 *   [0] in_fd        - Integer fd (>= 0), -1 for false/nil, IO was
 *                      resolved to its fd by the codegen, String path
 *                      was opened by the codegen
 *   [1] out_fd       - same conventions
 *   [2] err_fd       - same conventions
 *   [3] pgroup       - Integer (0=inherit, 1=new, >1=specific pgid)
 *   [4] rlimit_cpu   - Integer (seconds), nil = no limit
 *   [5] rlimit_as    - Integer (bytes), nil = no limit
 *   [6] chdir        - String path or nil
 *
 * IO and String values in opts[0..2] are resolved to fds by the
 * codegen (which has access to sp_File_fileno and open(2)). The
 * [:child, :out|:err|Integer] form is also resolved by the codegen
 * (it sees the literal array at parse time).
 */

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>       /* nanosleep: the cooperative wait's back-off */

#include "sp_alloc.h"   /* sp_PolyArray, sp_RbVal, sp_box_*, sp_raise_cls */
#include "sp_process_status.h"   /* sp_ProcessStatus, sp_box_process_status */

/* Local error-message builder. Returns a static buffer; copy the
   result before another call. Avoids sp_sprintf which would pull
   sp_class_to_s / sp_sym_to_s into the link. */
static SP_TLS char sp_err_buf[512];
static const char *sp_errf_errno(const char *prefix, int err) {
  snprintf(sp_err_buf, sizeof sp_err_buf, "%s - %s", prefix, strerror(err));
  return sp_err_buf;
}

/* Apply the redirect in the child: dup2 src_fd onto target_fd, close src.
   src_fd < 0 means the caller did not pass that slot in the opts hash
   (the codegen initialises every slot to -1 and overwrites only the ones
   the user set); in that case the fd is left inherited from the parent.
   CRuby semantics: an unset :in/:out/:err slot is inherit, not /dev/null.
   The previous /dev/null behaviour silently swallowed the child's stderr
   and made `Process.spawn("cc -c foo.c")` in verbose mode look like it
   produced no output. */
static void apply_redirect(int target_fd, int src_fd) {
  if (src_fd < 0) return;
  if (src_fd != target_fd) dup2(src_fd, target_fd);
}

/* Close the source descriptors, once every dup2 above has been made. Not
   inside apply_redirect: `out: f, err: f` names one descriptor twice, and
   closing it after the first redirect left the second dup2 working on a
   closed fd -- silently, so the child's stderr went nowhere (#4176). */
static void close_redirect_srcs(int in_fd, int out_fd, int err_fd) {
  int fds[3]; int n = 0;
  if (in_fd  > 2) fds[n++] = in_fd;
  if (out_fd > 2) fds[n++] = out_fd;
  if (err_fd > 2) fds[n++] = err_fd;
  for (int i = 0; i < n; i++) {
    int dup = 0;
    for (int j = 0; j < i; j++) if (fds[j] == fds[i]) dup = 1;
    if (!dup) close(fds[i]);
  }
}

/* Extract a resolved Integer fd from a pre-resolved opts slot. The
   codegen turns IO/String/false into Integer before passing; we
   just unbox. -1 means "not set" (/dev/null). */
static int slot_to_fd(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return -1;
  if (v.tag == SP_TAG_BOOL && v.v.i == 0) return -1;
  if (v.tag == SP_TAG_INT) return (int)v.v.i;
  sp_raise_cls("TypeError", "redirect slot must be Integer (codegen bug)");
  return -1;
}

sp_int sp_process_spawn(sp_RbVal cmd, sp_RbVal args_box,
                        sp_RbVal opts_box) {
  SP_GC_ROOT_RBVAL(cmd);
  SP_GC_ROOT_RBVAL(args_box);
  SP_GC_ROOT_RBVAL(opts_box);

  /* opts_box must be a PolyArray of 7 elements. */
  if (opts_box.tag != SP_TAG_OBJ ||
      opts_box.cls_id != SP_BUILTIN_POLY_ARRAY) {
    sp_raise_cls("TypeError", "opts must be a PolyArray (codegen bug)");
  }
  sp_PolyArray *opts = (sp_PolyArray *)opts_box.v.p;
  if (opts->len < 7) {
    sp_raise_cls("ArgumentError", "opts array too short (codegen bug)");
  }
  int in_fd  = slot_to_fd(opts->data[0]);
  int out_fd = slot_to_fd(opts->data[1]);
  int err_fd = slot_to_fd(opts->data[2]);

  int pgroup = 0;
  if (opts->data[3].tag == SP_TAG_NIL) pgroup = 0;
  else if (opts->data[3].tag == SP_TAG_BOOL && opts->data[3].v.i == 1) pgroup = 1;
  else if (opts->data[3].tag == SP_TAG_INT) pgroup = (int)opts->data[3].v.i;
  else sp_raise_cls("TypeError", "pgroup must be true, 0, or Integer");

  int rlimit_cpu_set = 0;
  rlim_t rlimit_cpu_val = 0;
  if (opts->data[4].tag == SP_TAG_INT) { rlimit_cpu_set = 1; rlimit_cpu_val = (rlim_t)opts->data[4].v.i; }
  else if (opts->data[4].tag != SP_TAG_NIL)
    sp_raise_cls("TypeError", "rlimit_cpu must be Integer");

  int rlimit_as_set = 0;
  rlim_t rlimit_as_val = 0;
  if (opts->data[5].tag == SP_TAG_INT) { rlimit_as_set = 1; rlimit_as_val = (rlim_t)opts->data[5].v.i; }
  else if (opts->data[5].tag != SP_TAG_NIL)
    sp_raise_cls("TypeError", "rlimit_as must be Integer");

  const char *chdir_to = NULL;
  if (opts->data[6].tag == SP_TAG_STR) chdir_to = opts->data[6].v.s;
  else if (opts->data[6].tag != SP_TAG_NIL)
    sp_raise_cls("TypeError", "chdir must be a String");

  /* Resolve cmd + args into argv. */
  const char *prog = NULL;
  char **argv = NULL;
  sp_PolyArray *cmd_arr = NULL;
  sp_PolyArray *args_arr = NULL;
  int extra_from_cmd = 0;
  int extra_from_args = 0;

  if (cmd.tag == SP_TAG_STR) {
    prog = cmd.v.s;
    if (args_box.tag == SP_TAG_OBJ &&
        args_box.cls_id == SP_BUILTIN_POLY_ARRAY) {
      args_arr = (sp_PolyArray *)args_box.v.p;
    } else if (args_box.tag != SP_TAG_NIL) {
      sp_raise_cls("TypeError", "args must be a PolyArray of extra args");
    }
  } else if (cmd.tag == SP_TAG_OBJ &&
             cmd.cls_id == SP_BUILTIN_POLY_ARRAY) {
    cmd_arr = (sp_PolyArray *)cmd.v.p;
    if (cmd_arr->len < 1) sp_raise_cls("ArgumentError", "empty command array");
    if (cmd_arr->data[0].tag != SP_TAG_STR)
      sp_raise_cls("ArgumentError", "command[0] must be a String");
    prog = cmd_arr->data[0].v.s;
    extra_from_cmd = cmd_arr->len - 1;
    if (args_box.tag == SP_TAG_OBJ &&
        args_box.cls_id == SP_BUILTIN_POLY_ARRAY) {
      args_arr = (sp_PolyArray *)args_box.v.p;
    }
  } else {
    sp_raise_cls("TypeError",
                 "wrong first argument type (expected String or Array)");
  }
  if (args_arr) extra_from_args = (int)args_arr->len;

  int total = 1 + extra_from_cmd + extra_from_args;
  argv = (char **)malloc(sizeof(char *) * (size_t)(total + 1));
  if (!argv) sp_raise_cls("NoMemoryError", "out of memory");
  argv[0] = (char *)prog;
  int ai = 1;
  if (cmd_arr) {
    for (int i = 1; i < cmd_arr->len; i++) {
      if (cmd_arr->data[i].tag != SP_TAG_STR)
        sp_raise_cls("ArgumentError", "command array element must be a String");
      argv[ai++] = (char *)cmd_arr->data[i].v.s;
    }
  }
  if (args_arr) {
    for (int i = 0; i < args_arr->len; i++) {
      if (args_arr->data[i].tag != SP_TAG_STR)
        sp_raise_cls("ArgumentError", "spawn args must be Strings");
      argv[ai++] = (char *)args_arr->data[i].v.s;
    }
  }
  argv[ai] = NULL;

  /* Pre-exec error pipe: the child writes the exec errno here if execve
     fails, so the parent can raise the matching Errno (CRuby raises
     Errno::ENOENT for "no such file or directory" instead of returning
     a dead pid). FD_CLOEXEC on the write end so a successful execvp
     closes it; the parent then sees a zero-byte read and no raise. */
  int err_pipe[2];
  if (pipe(err_pipe) < 0) {
    free(argv);
    sp_raise_cls("SystemCallError", sp_errf_errno("pipe failed", errno));
  }

  pid_t pid = fork();
  if (pid < 0) {
    free(argv);
    sp_raise_cls("SystemCallError", sp_errf_errno("fork failed", errno));
  }
  if (pid == 0) {
    /* CHILD. If execve fails, write the errno to the parent's pipe
       and exit 127; the parent will raise the matching Errno
       (Errno::ENOENT for "no such file") to match CRuby semantics.
       Using a pipe (not relying on the child's exit code alone)
       because the exit code is the same regardless of exec failure
       reason. */
    close(err_pipe[0]);
    if (fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC) < 0) { _exit(126); }
    if (chdir_to) {
      if (chdir(chdir_to) != 0) {
        int e = errno;
        (void)!write(err_pipe[1], &e, sizeof e);
        _exit(127);
      }
    }
    if (rlimit_cpu_set) {
      struct rlimit rl = { rlimit_cpu_val, RLIM_INFINITY };
      setrlimit(RLIMIT_CPU, &rl);
    }
    if (rlimit_as_set) {
      struct rlimit rl = { rlimit_as_val, RLIM_INFINITY };
      setrlimit(RLIMIT_AS, &rl);
    }
    if (pgroup == 1) {
      setpgid(0, 0);
    } else if (pgroup > 1) {
      setpgid(0, pgroup);
    }
    apply_redirect(0, in_fd);
    apply_redirect(1, out_fd);
    apply_redirect(2, err_fd);
    close_redirect_srcs(in_fd, out_fd, err_fd);
    execvp(prog, argv);
    /* exec returned: failure. Send the errno to the parent. */
    int e = errno;
    (void)!write(err_pipe[1], &e, sizeof e);
    _exit(127);
  }
  /* PARENT. Close the child's write end, read the errno if any. */
  close(err_pipe[1]);
  int exec_errno = 0;
  ssize_t got = read(err_pipe[0], &exec_errno, sizeof exec_errno);
  close(err_pipe[0]);
  free(argv);
  if (got > 0) {
    /* child failed to exec; the child has already exited 127, so the
       waitpid2 caller will still find a (zombie) pid, but we raise the
       CRuby-style exception first. The pid is leaked but harmless: the
       init process reaps the zombie. */
    errno = exec_errno;
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" :
                 errno == EACCES ? "Errno::EACCES" :
                 "SystemCallError",
                 sp_errf_errno("cannot execute", errno));
  }
  return (sp_int)pid;
}

/* ---- the child wait: a process facility, so it lives here and not in the
   scheduler. waitpid needs <sys/wait.h>; keeping it out of sp_sched.c leaves the
   scheduler buildable on a target with no process layer at all (wasm32-wasi
   links the plain scheduler for Thread.pass / Mutex / Queue while this TU is
   simply absent). The polling shape is the scheduler's concern and it exposes
   what this needs. Declared here rather than through sp_sched.h: see the note
   at the top on this TU's includes. */
extern int  sp_sched_other_threads_live(void);
extern void sp_Thread_pass(void);
#ifdef SP_THREADS
extern void sp_sched_sleep(double seconds);
#endif
/* Wait for one child without holding the OS worker while other green threads
   have to run. A blocking waitpid answers for the whole worker, and a started
   green thread is pinned to its worker, so `spin build --verbose` deadlocked:
   the parent waited for the compiler, the compiler filled the stderr pipe and
   blocked writing, and the reader thread that would have drained it could not
   be scheduled (#4381). The cooperative build has the same shape with one OS
   thread and several fibers.

   Poll and hand the scheduler back between attempts, but only while another
   thread is alive -- a program with one thread keeps the blocking wait and its
   exact wake-up. sp_Thread_pass runs a runnable sibling immediately; the sleep
   only keeps a busy loop from burning the core while the child works, and it
   caps at 5ms, which is nothing against a wait long enough to matter. */
int sp_sched_wait_child(int pid, int *status) {
  int r;
  if (!sp_sched_other_threads_live()) {
    do { r = (int)waitpid((pid_t)pid, status, 0); } while (r < 0 && errno == EINTR);
    return r;
  }
  { double back = 0.0002;
    for (;;) {
      do { r = (int)waitpid((pid_t)pid, status, WNOHANG); } while (r < 0 && errno == EINTR);
      if (r != 0) return r;   /* reaped, or an error for the caller to report */
      sp_Thread_pass();
#ifdef SP_THREADS
      sp_sched_sleep(back);
#else
      { struct timespec rq; rq.tv_sec = 0; rq.tv_nsec = (long)(back * 1e9);
        while (nanosleep(&rq, &rq) == -1 && errno == EINTR) {} }
#endif
      if (back < 0.005) back *= 2.0;
    } }
}

/* A blocking waitpid answers for the OS worker, and a started green thread is
   pinned to its worker, so blocking here stalls the thread that may have to
   drain this child's output before it can exit (#4381) -- hence the wait above.
*/
sp_PolyArray *sp_process_waitpid2(sp_int pid) {
  int status = 0;
  pid_t r = (pid_t)sp_sched_wait_child((int)pid, &status);
  if (r < 0) {
    if (errno == ECHILD) {
      sp_raise_cls("Errno::ECHILD", "No child processes");
    }
    sp_raise_cls("SystemCallError", sp_errf_errno("waitpid failed", errno));
  }
  sp_PolyArray *pa = sp_PolyArray_new();
  sp_PolyArray_push(pa, sp_box_int((sp_int)r));
  /* Second element is a Process::Status instance wrapping (pid, status),
     not a raw int -- .signaled? / .termsig dispatch on the boxed cls_id. */
  sp_PolyArray_push(pa, sp_box_process_status(sp_process_status_new((sp_int)r, (sp_int)status)));
  return pa;
}
