# Issue #640: `puts cond ? :sym : int` used to dispatch through
# the wrong puts variant (`sp_sym_to_s(an_int)` printing garbage
# instead of the int value). unify_return_type collapsed [int,
# symbol] to "symbol" via the "int-as-default" heuristic, so the
# else arm's int was passed through sym dispatch.
#
# Fix: compile_puts detects an IfNode arg whose then/else arms
# have different *concrete* base types (neither nil/void) and
# routes through sp_poly_puts so each arm's value is boxed to its
# real tag.
#
# nil-arm cases (`if cond; X; end` without else) are left alone —
# string and pointer types already handle nil through the existing
# str_or_nil path.

a = 2
puts a%2>0 ? :odd : a    # else: prints "2"

b = 3
puts b%2>0 ? :odd : b    # then: prints "odd"

# Float vs string
c = 1.5
puts c > 0 ? "positive" : c  # then: "positive"
puts c > 5 ? "big" : c       # else: 1.5
