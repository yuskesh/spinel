# More CPU-bound threads than OS workers, each a bounded loop with no explicit
# yield: under the parallel build the monitor must timeslice them (otherwise the
# excess threads starve), and the bound keeps the program correct and
# terminating under the cooperative single-thread gate build too. The final
# values are independent of the interleaving, so the output is deterministic.
ts = 8.times.map { |i| Thread.new { s = 0; 300000.times { s += 1 }; s + i } }
puts ts.map(&:value).inspect
