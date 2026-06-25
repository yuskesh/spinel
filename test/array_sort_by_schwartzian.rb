# sort_by lowers to a Schwartzian transform: the key block runs exactly ONCE per
# element, in element order (not once per comparison like a naive sort), the sort
# is stable, and the receiver is left unmodified.

# Once per element, in input order (route through a method to exercise the runtime
# path, not a constant fold).
def keyed_log(arr, log)
  arr.sort_by { |x| log << x; -x }
end
log = []
res = keyed_log([5, 3, 1, 4, 2], log)
p res          # sorted descending
p log          # the block saw each element once, in input order
p log.length   # 5, not 20

# Stability: equal keys keep their input order.
def by_first(pairs)
  pairs.sort_by { |pair| pair[0] }
end
p by_first([[1, "a"], [2, "b"], [1, "c"], [2, "d"], [1, "e"]])

# Float key.
def by_negf(arr)
  arr.sort_by { |x| x * -1.5 }
end
p by_negf([3, 1, 2])

# String key.
def by_self(arr)
  arr.sort_by { |s| s }
end
p by_self(["banana", "apple", "cherry"])

# Poly-array receiver, integer key derived from each element.
def by_digits(arr)
  arr.sort_by { |x| x.to_s.length }
end
p by_digits([100, 5, 42])

# Non-mutating: the receiver is unchanged after sort_by.
a = [3, 1, 2]
a.sort_by { |x| x }
p a

# Empty and single-element receivers.
def ident(arr)
  arr.sort_by { |x| x }
end
p ident([])
p ident([7])
