# Deeply nested break scopes: each recursion level enters a break-wrapped
# iterator whose scope stays live while the block recurses deeper, so many
# scopes are simultaneously on the fixed-size break stack. This exercises the
# push/pop balance and per-scope serial addressing at depth (well under the
# SP_BRK_STACK_MAX guard that turns an overflow into a loud fatal rather than
# corruption). deep(n) == n: each level's `each` returns its own break value.
def deep(n)
  return 0 if n == 0
  [1, 2, 3].each do |x|
    break deep(n - 1) + 1
  end
end
p deep(1)
p deep(40)

# Depth through a yield method: the spliced block's break targets the call that
# received it at each level, past the method's own iterator.
def relay
  [1].each { |x| yield x }
end
def climb(n)
  return 0 if n == 0
  relay { |x| break climb(n - 1) + x }
end
p climb(50)

# A break at a shallow level must still target its own scope when deeper
# (already-returned) scopes have been popped: sequential, not just nested.
def once
  [1, 2, 3].each { |x| break x * 10 if x == 2 }
end
sum = 0
10.times { sum += once }
p sum
