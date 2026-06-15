# A proc/lambda whose body's value is a Float carries that float back
# through `.call` (boxed in the poly return slot), instead of truncating
# to the int the proc-fn ABI slot defaults to.

# Direct literal receiver.
puts proc { 3.5 }.call
puts(-> { 4.25 }.call)

# Proc-variable receiver.
f = proc { 1.5 }
puts f.call
g = -> { 2.0 }
puts g.call

# Float-valued expression in the body.
h = proc { 1.5 + 2.0 }
puts h.call

# Float result used in arithmetic at the call site.
puts f.call + 0.5

# A method whose implicit-tail value is a float-returning proc call.
def tail_flt
  pr = proc { 6.25 }
  pr.call
end
puts tail_flt
