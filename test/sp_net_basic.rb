# sp_net.c -- ffi exposed to spinel programs.
#
# Cross-platform-deterministic smoke: shell_capture + getpid produce
# the same output on every POSIX target, so the .expected holds for
# Linux + macOS CI. (On Windows sp_net is stubbed -- the Makefile
# filters this test out there; the socket/poll/process surface is
# exercised by the consumer suites, e.g. tep, which run on POSIX.)
module Net
  ffi_func :sp_net_getpid,        [],           :int
  ffi_func :sp_net_shell_capture, [:str, :int], :str
end

# Shell capture: stdout of `printf hello` is exactly "hello".
puts Net.sp_net_shell_capture("printf hello", 64)

# getpid is always a positive pid (print a stable token, not the pid).
puts(Net.sp_net_getpid > 0 ? "pid-ok" : "pid-bad")
