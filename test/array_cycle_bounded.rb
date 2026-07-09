# <array>.cycle.first(n) / .take(n) consume the infinite cycle by a bounded count.
# Unbounded forms (cycle.to_a / .each / bare cycle) remain a compile-time reject so
# they can never hang -- see the note in the PR; they are intentionally not tested
# here (they do not compile).
p [1, 2, 3].cycle.first(7)
p [1, 2, 3].cycle.take(2)
p ["a", "b"].cycle.first(5)
p [10, 20, 30].cycle.first(0)
p [42].cycle.take(4)
# a negative count is an ArgumentError, matching Enumerable#first/#take
begin
  [1, 2, 3].cycle.first(-1)
rescue ArgumentError => e
  p e.message
end
begin
  [1, 2, 3].cycle.take(-2)
rescue ArgumentError => e
  p e.message
end
# the finite block form cycle(n) { } is unchanged
out = []
[1, 2].cycle(3) { |x| out << x }
p out
