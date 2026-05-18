# #584 (cielavenir). Multi-assignment used as an expression value
# whose RHS is a call returning a typed array (here `gets.split.
# map(&:to_i)` → int_array), followed by an outer subscript /
# comparison. Previously `compile_expr`'s MultiWriteNode arm only
# handled the ArrayNode-RHS shape (#554) and fell through to
# `return "0"` for the call-returning-array shape -- the outer
# `[0]` then lowered to `sp_IntArray_get(0, 0LL)` (NULL recv)
# and segv'd on the next iteration of any consumer loop.

def two_ints; [10, 20]; end
def three; [7, 8, 9]; end

# T1: call-returning-array RHS, value-of-expression form
result = (x, y = two_ints)
p x
p y
p result

# T2: subscript chain on the value
first = (p1, p2, p3 = three)[0]
p p1
p p2
p p3
p first

# T3: subscript chain on the value with comparison
def pair; [3, 4]; end
flag = (a, b = pair)[0] == 3
p a
p b
p flag
