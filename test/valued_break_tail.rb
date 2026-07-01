# A break-carrying iterator in a method's TAIL (return) position: its value --
# the break value, or the normal result when no break is taken -- IS the
# method's return value. (A plain iteration call at tail is a side-effect
# statement; a break-carrying one must instead flow through the value path.)

# self-returning iterator, break taken -> the break value
def each_brk;   [1, 2, 3].each { |x| break x * 100 if x == 2 }; end
p each_brk

# self-returning iterator, break NOT taken -> the receiver
def each_recv;  [1, 2, 3].each { |x| break x if x == 99 }; end
p each_recv

# each_with_index / each_with_object at tail
def ewi_brk;  %w[a b c].each_with_index { |s, i| break i if s == "b" }; end
p ewi_brk
def ewo_brk;  [1, 2, 3].each_with_object([]) { |x, o| break :bail if x == 2; o << x }; end
p ewo_brk

# value-producing iterators at tail, break taken
def map_brk;    [1, 2, 3].map    { |x| break :m if x == 2; x }; end
def select_brk; [1, 2, 3].select { |x| break 0  if x == 2; x.odd? }; end
def reject_brk; [1, 2, 3].reject { |x| break 7  if x == 2; false }; end
def reduce_brk; [1, 2, 3].reduce(0) { |a, x| break 77 if x == 2; a + x }; end
def find_brk;   [1, 2, 3].find   { |x| break(-1) if x == 2; false }; end
p map_brk; p select_brk; p reject_brk; p reduce_brk; p find_brk

# value-producing iterators at tail, break NOT taken -> the normal result
def map_normal; [1, 2, 3].map { |x| x * 2 }; end
p map_normal

# range iterator at tail
def rng_brk;    (1..5).map { |x| break x if x == 3; x }; end
p rng_brk

# a guard return before the tail iterator (the iterator is still the tail value)
def guarded(n)
  return :zero if n == 0
  [1, 2, 3].each { |x| break x * n if x == 2 }
end
p guarded(0)
p guarded(5)

# tail iterator through a user method that yields: the CALL's value is the
# break value even though the yield sits inside the method's own iterator
def relay
  [10, 20, 30].each { |x| yield x }
end
def via_relay
  relay { |x| break x + 1 if x == 20 }
end
p via_relay

# an if/else whose both tail branches are break-carrying iterators
def branchy(flag)
  if flag
    [1, 2].map { |x| break :t if x == 2; x }
  else
    [3, 4].each { |x| break x if x == 4 }
  end
end
p branchy(true)
p branchy(false)
