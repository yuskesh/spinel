# STDOUT/STDERR (constants) and $stdout/$stderr (globals) must actually write.
# Previously STDOUT.write / .puts / .print silently produced no output, and
# $stdout.write was a no-op returning nil. write returns the byte count.

n = STDOUT.write("hi\n")
p n                       # 3

STDOUT.puts "viaputs"     # viaputs
STDOUT.print "noeol"      # noeol
STDOUT.print "\n"

# Non-string args are stringified (Kernel#puts/#write coercion).
STDOUT.puts 42            # 42
m = STDOUT.write(99)
STDOUT.write("\n")
p m                       # 2

# The global forms behave the same.
g = $stdout.write("global\n")
p g                       # 7

# Multi-arg write returns the total bytes written.
t = $stdout.write("a", "b", "c")
$stdout.write("\n")
p t                       # 3

# STDERR goes to stderr (merged into output under 2>&1 in the harness).
STDERR.write("err\n")     # err
$stderr.puts "gerr"       # gerr
