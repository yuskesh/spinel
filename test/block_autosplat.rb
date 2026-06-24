# 2-param block auto-splat of array elements over `each` (W4 / B19).
# When a block with >=2 params iterates an array whose elements are arrays,
# Ruby splats each element into the params. A non-array element binds the
# first param (the rest nil). Receivers are routed through a method param so
# the array isn't constant-folded away.

# poly array of string/int pairs (full pairs fill both params)
def join_pairs(arr)
  out = []
  arr.each { |k, v| out << "#{k}#{v}" }
  out
end
p(join_pairs([["a", 1], ["b", 2]]))

# int pairs -> arithmetic on both params
def sum_pairs(arr)
  out = []
  arr.each { |a, b| out << a + b }
  out
end
p(sum_pairs([[1, 2], [3, 4]]))

# the whole pair is bound (k, v), not (pair, nil)
def echo_pairs(arr)
  out = []
  arr.each { |k, v| out << [k, v] }
  out
end
p(echo_pairs([["x", 9]]))

# three params over a 2-element pair: third param is nil
def triple(arr)
  out = []
  arr.each { |a, b, c| out << [a, b, c] }
  out
end
p(triple([["p", 1]]))

# a fully-poly receiver (mixed element kinds force the poly-value each path):
# array elements splat, scalar elements bind the first param with rest nil
def describe(arr)
  out = []
  arr.each { |a, b| out << "#{a.inspect},#{b.inspect}" }
  out
end
p(describe([["k", 1], "scalar", [2, 3]]))

# Hash#each with |k, v| still binds both (sanity: not regressed)
def pairs_of(h)
  out = []
  h.each { |k, v| out << "#{k}=#{v}" }
  out
end
p(pairs_of({ "a" => 1, "b" => 2 }))
