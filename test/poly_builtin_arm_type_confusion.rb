# A builtin pre-arm in front of the poly method dispatch must not unbox its
# runtime-shaped answer into a slot typed from the USER arm.
#
# `sp_poly_first` / `sp_poly_sum` / ... hand back a boxed sp_RbVal.  When a user
# class owns the name, analyze often types the call from that user method -- so
# the arm was emitted as `(sp_poly_first(v)).v.s` into a `const char *` slot.
# On a genuine Array that reads the boxed Integer 1 as a pointer and the first
# use dereferences it: SIGSEGV (rc=139), with the buffered stdout lost so it
# looked like "no output" rather than a crash.  Same shape for #hash, where the
# int from sp_rbval_hash_key was cast straight to `const char *`.
#
# Batch 6 (B12) then widened the result slot itself: when a user class owns the
# name, the builtin surface answers this receiver with a DIFFERENT type, AND the
# poly dispatch can actually grow a builtin arm for that name, the node becomes
# poly -- so the arm has somewhere to put its answer.  first / sum / max now
# print the builtin's value and match CRuby.
#
# #hash is the one pair still left, and the reason is the widen's own gate:
# `hash` is not a name whose poly dispatch can serve a builtin receiver
# (poly_builtin_arm_name_p), so the node is NOT widened and the call behaves
# exactly as it did before B12 -- `u.hash` reaches the user method through the
# switch, `42.hash` falls to the switch's default and raises.  Closing it needs
# the builtin FALLBACK in that default -- the other half of the family,
# deliberately not in batch 6.
#
# What this test pins either way: never a wild load.  A regression that turns
# any of these lines back into silence or rc=139 is the thing to catch.  Do not
# "fix" the remaining #hash line by making it match CRuby without fixing the
# rest of the family.
class U
  def first = "user-first"
  def sum   = "user-sum"
  def max   = "user-max"
  def hash  = "user-hash"
end

# The shape that pins the slot: one helper, two receiver types, so the
# parameter is poly but the CALL's result type is taken from the user method
# (a String).  With a block (`arr.each { |v| puts v.first }`) the slot widens to
# poly and the builtin arm already answers correctly -- that is the general fix
# this test does NOT make; see E048.
def show_first(x)
  puts x.first
rescue NoMethodError
  puts "first: NoMethodError"
end
def show_sum(x)
  puts x.sum
rescue NoMethodError
  puts "sum: NoMethodError"
end
def show_max(x)
  puts x.max
rescue NoMethodError
  puts "max: NoMethodError"
end
def show_hash(x)
  puts x.hash
rescue NoMethodError
  puts "hash: NoMethodError"
end

u = U.new
show_first(u)
show_first([1, 2, 3])
show_sum(u)
show_sum([1, 2, 3])
show_max(u)
show_max([3, 1, 2])
show_hash(u)
show_hash(42)
