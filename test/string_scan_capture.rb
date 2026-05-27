# Issue #880: String#scan returns nested arrays for capture groups.

puts "hello world".scan(/(\w+)/).inspect
puts "a1 b22".scan(/([a-z]+)(\d+)/).inspect
puts "hello world".scan(/\w+/).inspect
puts "ab ac".scan(/a(?:b|c)/).inspect
