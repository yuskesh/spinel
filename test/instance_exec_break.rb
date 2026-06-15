# `break` inside a direct instance_exec block exits the block. In
# expression position `break <v>` makes <v> the value of the call,
# matching CRuby. The body is spliced at the call site and wrapped in
# a `do { } while (0)`, so a top-level break lowers to a C break that
# carries the value; a break inside a nested loop binds to that loop
# instead and the instance_exec block runs on to its last expression.

class Box
  def initialize(v)
    @v = v
  end

  def val
    @v
  end
end

class BoxPlus < Box
end

b = BoxPlus.new(5)

# Expression position: break short-circuits with a value.
puts(b.instance_exec { break 7; 999 })              #=> 7

# Guard-style break, taken and not-taken.
puts(b.instance_exec { break 3 if val > 0; 999 })   #=> 3
puts(b.instance_exec { break 3 if val < 0; 999 })   #=> 999

# The break value reads the rebound self and a block argument.
puts(b.instance_exec(10) { |x| break val + x; 0 })  #=> 15

# A break inside a nested iterator binds to that iterator, not to the
# instance_exec block; the block continues to its last expression.
puts(b.instance_exec { [1, 2, 3].each { |x| break }; 42 })  #=> 42

# Statement position: a bare break is an early exit; the captured
# outer local keeps the value written before the break.
acc = 0
b.instance_exec { acc = 10; break; acc = 99 }
puts acc                                            #=> 10

puts "done"
