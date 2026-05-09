# Issue #396. A yield-bearing top-level fn called with a literal
# block + the result assigned (`x = measure { ... }`) emitted
# `sp_measure(, NULL, NULL)` -- empty leading slot + extra trailing
# NULL, fails C-compile.
#
# Fix: compile_no_recv_call_expr now detects yield-method + literal-
# block in expression context and routes to compile_yield_call_expr
# (the expression-context counterpart of compile_yield_call_stmt).
# The body is inlined and the last stmt's value is captured into a
# result temp.
#
# Three working variants stayed working: assignment-less stmt form
# (already inlined), `&blk` param form (regular block forwarding),
# and yield-method with no block at the call site (yargs path).

def measure
  yield
  42
end

x = measure { puts "block" }
puts x.to_s         # 42

# Last stmt is a local read (after locals + yield).
def assign_last
  yield
  z = 99
  z
end

w = assign_last { puts "hi" }
puts w.to_s         # 99

# String return type
def str_call
  yield
  "result"
end

s = str_call { puts "in" }
puts s
