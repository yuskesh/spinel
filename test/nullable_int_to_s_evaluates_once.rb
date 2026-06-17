# `<nullable int>.to_s` / `.inspect` must evaluate its receiver exactly once.
# It lowers to a test-then-convert (`x == SP_INT_NIL ? "" : to_s(x)`); a naive
# emit inlines the receiver twice, so a side-effecting receiver (a counter
# bump, an I/O read) runs twice and yields the wrong value.
$n = 0
def step
  $n = $n + 1
  $n
end

puts step.to_s        # 1
puts step.to_s        # 2
puts step.inspect     # 3
puts $n.to_s          # 3 -- exactly three bumps, not six
