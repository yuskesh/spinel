#!/bin/bash
# tsan-run.sh -- compile and run a threaded spinel program under ThreadSanitizer.
#
# The normal test gate links the single-threaded archive even for threaded tests
# (test-run does its own cc with libspinel_rt.a and no -DSP_THREADS), so it never
# exercises the mt archive's parallel paths. This drives a program through the
# *threaded* runtime instead, instrumented with TSan, so a data race on the
# shared GC heap, the thread registry, or the run queue is reported instead of
# silently corrupting memory. This is the Phase 1 (N>1) validation gate.
#
# Usage: scripts/tsan-run.sh prog.rb [prog args...]
#   SPINEL=path/to/spinel  to override the compiler binary
#   SPINEL_WORKERS=N       to set the worker count (once N>1 lands)
set -euo pipefail

rb="${1:?usage: tsan-run.sh prog.rb [args...]}"; shift || true
SPINEL="${SPINEL:-bin/spinel}"
ARCHIVE="lib/libspinel_rt_mt_tsan.a"

if [ ! -f "$ARCHIVE" ]; then
  echo "tsan-run: $ARCHIVE missing -- run 'make tsan-archive' first" >&2
  exit 2
fi

c="$(mktemp /tmp/tsan-run-XXXXXX.c)"
bin="$(mktemp /tmp/tsan-run-XXXXXX.bin)"
trap 'rm -f "$c" "$bin"' EXIT

"$SPINEL" "$rb" -S > "$c"
# -Wno-all matches the production cc driver (src/main.c): the generated C
# carries benign patterns (e.g. a fiber body's dead deferred-return epilogue)
# that newer clangs otherwise reject by default.
cc -O1 -g -Wno-all -fsanitize=thread -DSP_THREADS -ftls-model=initial-exec \
   -Ilib -Ilib/regexp "$c" "$ARCHIVE" -lm -lpthread -o "$bin"

# halt_on_error keeps the first race fatal (a clean exit means TSan saw none).
TSAN_OPTIONS="halt_on_error=1 ${TSAN_OPTIONS:-}" "$bin" "$@"
