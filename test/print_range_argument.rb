# p / puts / print previously fell through to the unsupported-argument
# diagnostic when handed a Range. Integer ranges render as "first..last"
# ("first...last" when the end is excluded). Receivers are exercised both as
# literals and through a method parameter so the value path is covered.

p 1..3
p 1...5
p(-3..-1)
p 5..5

x = 10..20
p x

puts 1..3
print 1..3
puts

def show(r)
  p r
  puts r
  print r
  puts
end

show(2..8)
show(7...7)
