require 'set'

s = Set.new
s.add(1)
s.add(2)
s << 3
s.add(2)
puts s.size
puts s.include?(2)

s.delete(2)
puts s.include?(2)
puts s.size

# deleting a missing key is a no-op and still returns the set (chainable)
s.delete(99).add(4)
puts s.size
puts s.include?(4)
puts s.member?(1)
puts s.empty?
