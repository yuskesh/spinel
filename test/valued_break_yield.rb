# A `break <v>` inside a block propagates through user methods that yield
# (CRuby's TAG_BREAK): the CALL that received the block returns <v>, however
# deep the yield sits -- inside the method's own loops or nested iterators.
def m1; yield; end
p(m1 { break 5 })
p(m1 { 3 })

# a no-value method body: bare result is nil
def m2; yield; nil; end
p(m2 { break :v })

# yield inside the method's own while loop
def m3; i = 0; while i < 3; i += 1; yield i; end; :done; end
p(m3 { |i| break i * 10 if i == 2 })
p(m3 { |i| i })

# yield inside a nested iterator: the break still exits the OUTER call
def m4; [1, 2, 3].each { |x| yield x }; end
p(m4 { |x| break x + 100 if x == 2 })

# nested WRAPPED iterator: the inner map has its own break scope, yet the
# spliced outer block's break targets m5's call (serial addressing)
def m5; [1, 2].map { |x| break :inner if x > 99; yield x }; end
p(m5 { |x| break :outer if x == 2; x })
p(m5 { |x| x * 2 })

# receiver-bearing user method: the no-break result is the METHOD's value,
# not the receiver
class Bag
  def initialize(xs); @xs = xs; end
  def each
    i = 0
    while i < @xs.length
      yield @xs[i]
      i += 1
    end
    :bag_done
  end
end
b = Bag.new([1, 2, 3])
p(b.each { |x| break x * 7 if x == 2 })
p(b.each { |x| x })

# statement position
m1 { break 7 }
puts "after"

# statement-position nesting: the inner each's break must not escape to the
# outer map
p [1, 2, 3].map { |x| break 1 if x == 9; [5, 6].each { break }; x }
