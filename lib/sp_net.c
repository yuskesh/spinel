/* sp_net.c -- POSIX TCP / poll / process / shell primitives.
 * See sp_net.h. Pure C, no spinel-runtime dependency; no OpenSSL.
 *
 * Extracted from tep's lib/tep/sphttp.c (the POSIX-runtime core that
 * is generic across HTTP-shaped Spinel programs), per matz/spinel#1054
 * and OriPekelman/tep#12. HTTP framing + WebSocket accessors + TLS stay
 * framework-side.
 *
 * sp_net is a POSIX prefork/poll runtime (sockets, poll(2), fork,
 * sigaction). */
#include "sp_net.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>

#define SP_NET_BUFSIZE 65536

/* ---------- graceful shutdown ---------- */

static volatile sig_atomic_t sp_net_term_flag = 0;
/* Self-pipe: the term handler writes a byte here so a blocked accept (via
 * poll) wakes even when the signal lands in the check-then-accept window. */
static int sp_net_sigpipe[2] = {-1, -1};

static void sp_net_term_signal(int sig) {
    (void)sig;
    sp_net_term_flag = 1;
    if (sp_net_sigpipe[1] >= 0) {
        ssize_t w = write(sp_net_sigpipe[1], "x", 1);  /* write() is async-signal-safe */
        (void)w;
    }
}

int sp_net_install_term_handlers(void) {
    if (sp_net_sigpipe[0] < 0 && pipe(sp_net_sigpipe) == 0) {
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(sp_net_sigpipe[i], F_GETFL, 0);
            if (fl >= 0) fcntl(sp_net_sigpipe[i], F_SETFL, fl | O_NONBLOCK);
            int fd = fcntl(sp_net_sigpipe[i], F_GETFD, 0);
            if (fd >= 0) fcntl(sp_net_sigpipe[i], F_SETFD, fd | FD_CLOEXEC);
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sp_net_term_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESETHAND and no SA_RESTART: a repeated term signal must keep
     * hitting this handler (idempotent flag set) rather than the default
     * action -- the latter killed the process with 143 instead of letting
     * the accept loop exit cleanly. Without SA_RESTART, a signal during a
     * blocked accept/poll surfaces as EINTR. */
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    return 0;
}

int sp_net_shutdown_requested(void) {
    return (int)sp_net_term_flag;
}

/* Byte count written into the recv buffer by the most recent recv. The FFI
 * `:binstr` return mode reads this to build a binary-safe String of exactly
 * this many bytes (rather than strlen-truncating at the first NUL), so the same
 * recv functions serve both text (`:str`) and binary (`:binstr`) callers.
 * Single-threaded (the Fiber model is cooperative), so no locking is needed. */
int sp_net_bin_len = 0;

/* Disable Nagle on a connection fd. TCP_NODELAY is a per-connection option, so
 * it must be set on each accepted fd, not just the listener -- otherwise small
 * writes (response headers, WebSocket frames) stall against delayed-ACK. */
void sp_net_set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* ---------- TCP socket lifecycle ---------- */

int sp_net_listen(int port, int reuseport) {
    if (port < 0 || port > 65535) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    if (reuseport) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    }
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* Don't die on a write to a peer that closed. */
    signal(SIGPIPE, SIG_IGN);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 1024) < 0) { close(fd); return -1; }
    return fd;
}

#ifdef SP_THREADS
int sp_sched_wait_io(int fd, short events);   /* lib/sp_sched.c: scheduler-aware I/O wait */
#endif
int sp_net_accept(int sfd) {
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int fd;
#ifdef SP_THREADS
    /* Scheduler-aware accept: the listen fd is non-blocking, so accept returns
       EAGAIN when no connection is pending and the green thread parks (freeing
       its worker) until the monitor's poll reports the fd readable. Idempotent. */
    sp_net_set_nonblock(sfd);
    for (;;) {
        if (sp_net_term_flag) return -1;
        fd = accept(sfd, (struct sockaddr *)&caddr, &clen);
        /* The connection fd is non-blocking too, so recv/send on it park the
           green thread (scheduler-aware) instead of blocking the OS worker. */
        if (fd >= 0) { sp_net_set_nodelay(fd); sp_net_set_nonblock(fd); return fd; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!sp_sched_wait_io(sfd, POLLIN)) return -1;
            continue;
        }
        return -1;
    }
#else
    for (;;) {
        if (sp_net_term_flag) return -1;
        /* Wait for an incoming connection OR a term signal (which wakes the
         * self-pipe). Polling first closes the check-then-block race: a
         * signal that set the flag/pipe before we block makes poll return
         * immediately, so we never enter a blocking accept with the flag
         * already set. */
        struct pollfd pfds[2];
        pfds[0].fd = sfd;               pfds[0].events = POLLIN; pfds[0].revents = 0;
        nfds_t npfd = 1;
        if (sp_net_sigpipe[0] >= 0) {
            pfds[1].fd = sp_net_sigpipe[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;
            npfd = 2;
        }
        int pr = poll(pfds, npfd, -1);
        if (sp_net_term_flag) return -1;
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!(pfds[0].revents & POLLIN)) continue;  /* woken by the self-pipe alone */
        fd = accept(sfd, (struct sockaddr *)&caddr, &clen);
        if (fd >= 0) { sp_net_set_nodelay(fd); return fd; }
        if (errno == EINTR) {
            if (sp_net_term_flag) return -1;
            continue;   /* unrelated signal (SIGCHLD, ...) -- retry */
        }
        return -1;
    }
#endif
}

int sp_net_accept_nb(int sfd) {
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int fd;
    do {
        fd = accept(sfd, (struct sockaddr *)&caddr, &clen);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) sp_net_set_nodelay(fd);
    return fd;
}

int sp_net_connect(const char *host, int port) {
    if (port < 0 || port > 65535) return -1;
    /* A client that never listens still needs SIGPIPE ignored: a write
     * to an upstream that closed would otherwise kill the process. */
    signal(SIGPIPE, SIG_IGN);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    if (getaddrinfo(host, portbuf, &hints, &res) != 0) return -1;

    int fd = -1;
    struct addrinfo *ai;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SP_THREADS
    /* Non-blocking so recv/send park the green thread instead of blocking the
       worker (the connect itself stays blocking -- it completes promptly). */
    sp_net_set_nonblock(fd);
#endif
    return fd;
}

int sp_net_close(int fd) {
    return close(fd);
}

int sp_net_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------- TCP I/O ---------- */

/* Wait until `fd` is ready for `events` (POLLIN for a read, POLLOUT for a
   write) before retrying a syscall that returned EAGAIN/EWOULDBLOCK. The
   accepted client fds are non-blocking, so a recv/send can would-block even
   after the scheduler's poll reported the fd ready (a transient kernel state,
   a partial drain, etc.); treating that as EOF/error wrongly tears down a live
   connection (#1500). Block on the single fd rather than busy-spinning, and
   recheck the term flag on the 1s timeout so a shutdown still breaks the wait.
   Returns 1 to retry the syscall, 0 to give up (term requested or poll error). */
static int sp_net_wait_io(int fd, short events) {
    if (sp_net_term_flag) return 0;
#ifdef SP_THREADS
    /* Scheduler-aware: park this green thread and hand its OS worker to another
       thread until the fd is ready, rather than blocking the worker in poll. */
    return sp_sched_wait_io(fd, events);
#else
    for (;;) {
        if (sp_net_term_flag) return 0;
        struct pollfd pf;
        pf.fd = fd; pf.events = events; pf.revents = 0;
        int pr = poll(&pf, 1, 1000);
        if (pr > 0) return 1;
        if (pr == 0) continue;            /* timeout: recheck term, keep waiting */
        if (errno == EINTR) continue;
        return 0;
    }
#endif
}

int sp_net_write_str(int fd, const char *s) {
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, s + off, len - off, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && sp_net_wait_io(fd, POLLOUT)) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int sp_net_write_bytes(int fd, const char *data, int n) {
    size_t total = (n < 0) ? 0 : (size_t)n;
    size_t off = 0;
    while (off < total) {
        ssize_t w = send(fd, data + off, total - off, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && sp_net_wait_io(fd, POLLOUT)) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static char sp_net_recv_buf[SP_NET_BUFSIZE];
const char *sp_net_recv_some(int fd, int maxlen) {
    if (maxlen <= 0 || maxlen >= SP_NET_BUFSIZE) maxlen = SP_NET_BUFSIZE - 1;
    ssize_t n;
    for (;;) {
        /* Cooperative shutdown: bail rather than retry into the signal. */
        if (sp_net_term_flag) {
            sp_net_bin_len = 0;
            sp_net_recv_buf[0] = '\0';
            return sp_net_recv_buf;
        }
        n = recv(fd, sp_net_recv_buf, (size_t)maxlen, 0);
        if (n >= 0) break;
        if (errno == EINTR) continue;   /* e.g. SIGCHLD in a prefork server */
        /* Non-blocking fd would-block: wait for readability and retry rather
           than reporting an empty result the caller can't tell from EOF. */
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && sp_net_wait_io(fd, POLLIN)) continue;
        sp_net_bin_len = 0;
        sp_net_recv_buf[0] = '\0';
        return sp_net_recv_buf;
    }
    sp_net_bin_len = (int)n;            /* exact byte count for the :binstr path */
    sp_net_recv_buf[n] = '\0';
    return sp_net_recv_buf;
}

static char sp_net_recv_all_buf[SP_NET_BUFSIZE];
const char *sp_net_recv_all(int fd, int max_bytes) {
    if (max_bytes <= 0 || max_bytes >= SP_NET_BUFSIZE) max_bytes = SP_NET_BUFSIZE - 1;
    int total = 0;
    while (total < max_bytes) {
        if (sp_net_term_flag) break;
        ssize_t n = recv(fd, sp_net_recv_all_buf + total, (size_t)(max_bytes - total), 0);
        if (n < 0) {
            if (errno == EINTR) continue;   /* retry rather than truncate */
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && sp_net_wait_io(fd, POLLIN)) continue;
            break;
        }
        if (n == 0) break;                   /* clean EOF */
        total += (int)n;
    }
    sp_net_bin_len = total;              /* exact byte count for the :binstr path */
    sp_net_recv_all_buf[total] = '\0';
    return sp_net_recv_all_buf;
}

/* ---------- poll(2) ---------- */

#define SP_NET_POLL_MAX 256
static struct pollfd sp_net_poll_set[SP_NET_POLL_MAX];
static int           sp_net_poll_n = 0;

int sp_net_poll_reset(void) {
    sp_net_poll_n = 0;
    return 0;
}

int sp_net_poll_add(int fd, int mode_bits) {
    if (sp_net_poll_n >= SP_NET_POLL_MAX) return -1;
    short ev = 0;
    if (mode_bits & 1) ev |= POLLIN;
    if (mode_bits & 2) ev |= POLLOUT;
    sp_net_poll_set[sp_net_poll_n].fd      = fd;
    sp_net_poll_set[sp_net_poll_n].events  = ev;
    sp_net_poll_set[sp_net_poll_n].revents = 0;
    return sp_net_poll_n++;
}

int sp_net_poll_run(int timeout_ms) {
    int r;
    for (;;) {
        /* Don't retry into a pending shutdown -- surface it as -1 so a
         * blocked scheduler tick can break and run shutdown hooks. */
        if (sp_net_term_flag) return -1;
        r = poll(sp_net_poll_set, sp_net_poll_n, timeout_ms);
        if (r >= 0) return r;
        if (errno == EINTR) {
            if (sp_net_term_flag) return -1;
            continue;
        }
        return -1;
    }
}

int sp_net_poll_ready(int slot) {
    if (slot < 0 || slot >= sp_net_poll_n) return 0;
    short rev = sp_net_poll_set[slot].revents;
    int out = 0;
    if (rev & (POLLIN | POLLHUP | POLLERR)) out |= 1;
    if (rev & POLLOUT)                      out |= 2;
    return out;
}

/* ---------- process (prefork) ---------- */

int sp_net_fork(void) {
    return (int)fork();
}

int sp_net_exit(int status) {
    _exit(status);
    return 0;   /* unreachable */
}

int sp_net_getpid(void) {
    return (int)getpid();
}

int sp_net_wait_any(void) {
    int status = 0;
    pid_t p = wait(&status);
    return (int)p;
}

/* ---------- shell ---------- */

static char sp_net_shell_buf[SP_NET_BUFSIZE];
const char *sp_net_shell_capture(const char *cmd, int max_bytes) {
    if (max_bytes <= 0 || max_bytes >= SP_NET_BUFSIZE) max_bytes = SP_NET_BUFSIZE - 1;
    sp_net_shell_buf[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return sp_net_shell_buf;
    size_t total = 0;
    while (total < (size_t)max_bytes) {
        size_t n = fread(sp_net_shell_buf + total, 1, (size_t)max_bytes - total, fp);
        if (n == 0) break;
        total += n;
    }
    sp_net_bin_len = (int)total;        /* exact byte count for the :binstr path */
    sp_net_shell_buf[total] = '\0';
    pclose(fp);
    return sp_net_shell_buf;
}
