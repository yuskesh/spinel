# Thread.list returns the main thread plus every live spawned thread. At N=1 a
# spawned thread does not run until the current thread yields, so the counts
# here are deterministic.
p Thread.list.size                       # 1 (just main)
p Thread.list.include?(Thread.current)   # true

threads = (1..3).map { Thread.new { 1 } }
p Thread.list.size                       # 4 (main + 3 runnable)

threads.each(&:join)
p Thread.list.size                       # 1 (the spawned threads finished)
p Thread.list.include?(Thread.main)      # true
