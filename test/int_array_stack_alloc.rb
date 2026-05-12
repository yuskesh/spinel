# Stack-allocated sp_IntArray for escape-safe locals. When a method
# creates an int_array local via `[]` literal and uses it only with
# built-in IntArray ops (push, length, [], etc.) or whitelisted user
# methods (push_child_ids), the codegen emits the local as a stack-
# allocated sp_IntArray struct instead of going through
# sp_IntArray_new + sp_gc_alloc.
#
# The wrapper struct places 8 bytes of padding before the
# sp_IntArray with the last byte set to 0xff so sp_gc_mark treats
# any GC root that happens to point at this struct (via a callee's
# SP_GC_ROOT(param) registration) as an immutable static object —
# no marking or scanning, no GC bookkeeping.
#
# Coverage:
# - Simple push loop with growth past initial cap (16) so the
#   no-GC-header realloc path in sp_IntArray_push_grow runs.
# - Pop / shift to verify start/len bookkeeping stays correct on
#   stack-alloc'd structs.
# - Pass to a user method that mutates via push (the canonical
#   accumulator pattern) — push_child_ids is whitelisted in
#   stack_ctx_safe_arg_method but we verify the general shape works.

def build
  cs = []
  i = 0
  while i < 50
    cs.push(i * 7)
    i = i + 1
  end
  total = 0
  cs.each do |v|
    total = total + v
  end
  total
end

def first_n_after_pop
  cs = []
  i = 0
  while i < 10
    cs.push(i)
    i = i + 1
  end
  cs.pop
  cs.pop
  cs.length
end

def first_after_shift
  cs = []
  cs.push(100)
  cs.push(200)
  cs.push(300)
  cs.shift
  cs[0]
end

puts build                     # 50 * 49 / 2 * 7 = 8575
puts first_n_after_pop         # 8 (10 pushed - 2 popped)
puts first_after_shift         # 200

# Stack-alloc + each block: receiver references inside an `each`
# block stay safe (block body is inlined at the call site, no
# closure capture). The walker now descends into block bodies of
# sync-block-taking methods (each / map / inject / etc.) with the
# same context-based escape checks as the top-level body.
def double_sum
  vs = []
  vs.push(1)
  vs.push(2)
  vs.push(3)
  acc = 0
  vs.each do |v|
    acc = acc + v + v
  end
  acc
end

# Stack-alloc + []= sparse write: previously rejected because
# `sp_IntArray_set_slow`'s grow path touched the GC header. Now
# the helper checks is_stack and skips the bookkeeping, so sparse
# `arr[i] = v` writes work on stack-alloc'd arrays too.
def sparse_set
  arr = []
  arr[5] = 42
  arr[10] = 99
  arr.length
end

# Stack-alloc + unshift: same shape as above for the unshift path.
def unshift_pattern
  arr = []
  arr.push(2)
  arr.push(3)
  arr.unshift(1)
  arr[0]
end

puts double_sum                # 12 (sum 1+2+3 doubled)
puts sparse_set                # 11 (indices 0..10 → length 11)
puts unshift_pattern           # 1
