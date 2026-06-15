# `next` inside a direct instance_exec block exits the block once. In
# expression position `next <v>` makes <v> the value of the call,
# matching CRuby -- equivalent to `break <v>` for a once-run block. The
# body is spliced at the call site and wrapped in a `do { } while (0)`,
# so a top-level `next` lowers to a C `continue` that falls through the
# `while (0)` and exits with that value; a `next` inside a nested loop
# binds to that loop instead and the block runs on to its last
# expression.

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

# Expression position: next short-circuits with a value.
puts(b.instance_exec { next 3; 999 })               #=> 3

# Guard-style next, taken and not-taken.
puts(b.instance_exec { next val + 1 if val > 0; 999 })  #=> 6
puts(b.instance_exec { next 7 if val < 0; 999 })        #=> 999

# The next value reads the rebound self and a block argument.
puts(b.instance_exec(10) { |x| next val + x; 0 })   #=> 15

# A next inside a nested iterator binds to that iterator, not to the
# instance_exec block; the block continues to its last expression.
puts(b.instance_exec { [1, 2, 3].each { |x| next }; 42 })  #=> 42

# Statement position: a bare next is an early exit; the captured outer
# local keeps the value written before the next.
acc = 0
b.instance_exec { acc = 10; next; acc = 99 }
puts acc                                            #=> 10

puts "done"
