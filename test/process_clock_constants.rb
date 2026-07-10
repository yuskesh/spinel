# Process::CLOCK_MONOTONIC / CLOCK_REALTIME are Integer values, usable
# beyond a direct clock_gettime argument.
x = Process::CLOCK_MONOTONIC
puts x.nil?
puts Process::CLOCK_MONOTONIC.nil?
puts Process::CLOCK_REALTIME.is_a?(Integer)
clk = Process::CLOCK_MONOTONIC
puts Process.clock_gettime(clk).is_a?(Float)
