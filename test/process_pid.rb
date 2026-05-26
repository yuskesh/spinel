# Issue #893: Process.pid / Process.ppid dispatched via getpid /
# getppid. Values vary by run; test the type and that they're
# positive.
puts Process.pid > 0
puts Process.ppid > 0
