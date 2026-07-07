# Struct and Data support pattern matching: #deconstruct (array patterns)
# and #deconstruct_keys (hash patterns). Neither has an explicit method;
# the compiler synthesizes the member array / member hash inline.
S = Struct.new(:x, :y, :z)
D = Data.define(:a, :b)

# case/in, array pattern
case S.new(1, 2, 3)
in [a, b, c]
  p [a, b, c]
end

# case/in, hash pattern (subset of keys)
case S.new(10, 20, 30)
in { x:, z: }
  p [x, z]
end

# case/in with a trailing splat
case S.new(1, 2, 3)
in [first, *rest]
  p [first, rest]
end

# type guards inside the pattern
case S.new(1, 2, 3)
in [Integer => p1, Integer, Integer]
  puts p1
end

# Data: array + hash patterns
case D.new(4, 5)
in [a, b]
  p [a, b]
end
case D.new(6, 7)
in { a:, b: }
  p [a, b]
end

# one-line rightward assignment
S.new(8, 9, 10) => [q, r, s]
p [q, r, s]
D.new(11, 12) => { a:, b: }
p [a, b]

# non-matching arm falls through
result =
  case S.new(1, 2, 3)
  in [0, _, _] then "zero"
  in [1, _, _] then "one"
  else "other"
  end
puts result
