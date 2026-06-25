# A non-lambda proc's `return` returns from the method that created the proc
# (CRuby non-local return); a lambda's `return` is local to the lambda.
def m
  proc { return 30 }.call
  40
end
p m

def lam
  -> { return 30 }.call
  40
end
p lam

# the non-local return aborts the rest of the method
def compute
  r = proc { return 7 }.call
  r + 100
end
p compute

# heap (string) return value
def smethod
  proc { return "early" }.call
  "late"
end
p smethod

# bare return yields nil
def bare
  proc { return }.call
  "after"
end
p bare.inspect

# conditional: returns non-locally only when the guard holds
def cond(x)
  proc { return "yes" if x > 0 }.call
  "no"
end
p cond(5)
p cond(-1)

# Proc.new form
def pnew
  Proc.new { return 99 }.call
  -1
end
p pnew

# Recursion: each invocation owns its own proc-return frame, so the return
# targets the correct method instance.
def countdown(n, acc)
  proc { return acc if n == 0 }.call
  countdown(n - 1, acc + n)
end
p countdown(5, 0)

# Nested returning-proc methods: inner's return unwinds only inner.
def inner
  proc { return "inner-ret" }.call
  "inner-normal"
end
def outer
  x = inner
  proc { return "outer:#{x}" }.call
  "outer-normal"
end
p outer

# Multi-value return becomes an array.
def mv
  proc { return 1, 2, 3 }.call
  [9]
end
p mv

# Many sequential calls: each non-local return (and each fall-through) pops its
# frame, so the bounded frame stack never overflows.
def doubled(x)
  proc { return x * 2 }.call
  -1
end
def maybe(x)
  proc { return 100 if x.even? }.call
  1
end
td = 0; ts = 0
500.times { |i| td += doubled(i); ts += maybe(i) }
p td
p ts

# Fall-through value: a returning proc that does NOT return still yields its tail
# expression (the `.call` result is the method's value). Mixed return/tail types
# unify to a poly home; same-type stays scalar.
def tail_or_ret(n)
  proc { return 99 if n < 0; "tail" }.call
end
p tail_or_ret(5)
p tail_or_ret(-1)

def tail_int(n)
  proc { return 5 if n < 0; 10 }.call
end
p tail_int(1)
p tail_int(-1)

# A `return` inside an inlined iteration block is non-local: it returns from the
# home method, not just the block, so the trailing statement is skipped.
def first_even_times_ten
  proc { [1, 2, 3].each { |x| return x * 10 if x == 2 }; :fell }.call
  :after
end
p first_even_times_ten

# Inlined block that falls through (no return fires) -- its tail is the value.
def all_fall_through
  proc { [1, 2, 3].each { |x| x * 10 }; :fell }.call
end
p all_fall_through

# Block-body local plus a non-local return from inside the block.
def block_local_return
  proc { [1, 2, 3].each { |x| y = x * 10; return y if x == 2 }; :fell }.call
end
p block_local_return

# each_with_index inside a returning proc.
def index_of_twenty
  proc { [10, 20, 30].each_with_index { |v, i| return i if v == 20 }; -1 }.call
end
p index_of_twenty

# map (a collecting block) inside a proc, no return.
def doubled_array
  proc { [1, 2, 3].map { |x| x * 2 } }.call
end
p doubled_array

# Nested inlined blocks (block within block) with a non-local return.
def nested_blocks_return
  proc { [1, 2].each { |a| [3, 4].each { |b| return a * b if a == 2 && b == 4 } }; 0 }.call
end
p nested_blocks_return

# Sibling inlined blocks reusing a param name share one declaration.
def sibling_param_reuse
  proc { [1, 2].each { |x| return x if x == 2 }; [3, 4].each { |x| return x }; 0 }.call
end
p sibling_param_reuse

# Deep recursion through a returning-proc method: the frame stack grows past the
# old fixed cap (256) rather than raising SystemStackError.
def countdown_to_zero(n)
  return n if n <= 0
  proc { return countdown_to_zero(n - 1) }.call
end
p countdown_to_zero(2000)

# A fiber whose body contains an inlined block: the block params/locals are
# collected for the fiber function (parallels the proc collection fix).
fib = Fiber.new do
  total = 0
  [1, 2, 3].each { |x| total += x }
  Fiber.yield total
end
p fib.resume
