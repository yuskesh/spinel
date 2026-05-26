class A
  X = 1
  Y = "two"
end

puts A.const_defined?(:X)
puts A.const_defined?(:Y)
puts A.const_defined?(:Z)

class B
end

puts B.const_defined?(:W)
