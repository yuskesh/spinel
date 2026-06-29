# Kernel#warn(category:) honors CRuby's default warning levels. The :deprecated
# category is off by default (Warning[:deprecated] == false without -W:deprecated),
# so those warnings print nothing; other categories print normally. Suppressed
# messages are still evaluated for their side effects.
warn("plain warning")
warn("deprecated thing", category: :deprecated)
warn("experimental thing", category: :experimental)
warn("a", "b", category: :deprecated)
warn("c", "d", category: :experimental)

# the message expression of a suppressed warning is still evaluated
def shout; $stdout.puts "evaluated"; "msg"; end
warn(shout, category: :deprecated)

$stdout.puts "done"
