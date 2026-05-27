# Comparable#clamp — string receiver and Range arg form.
# 2-arg int form was already supported by #899.
puts "b".clamp("a", "c")
puts "z".clamp("a", "c")
puts "a".clamp("b", "y")

# Range arg on Integer
puts 5.clamp(1..10)
puts 0.clamp(1..10)
puts 11.clamp(1..10)

# Range arg on String
puts "m".clamp("a".."z")
puts "Z".clamp("a".."z")
