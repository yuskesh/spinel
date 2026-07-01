# find / detect { |x| cond } over a poly array (mixed element types, or an array
# of user objects) returns the first matching element, or nil when none match.
# The typed-array forms already worked; array_kind is NULL for a poly array, so
# this exercises the boxed-element path. Distinct helpers keep each receiver's
# element type clean and defeat constant folding of the receiver.
def sa(x) = x
def sb(x) = x
def sc(x) = x
def su(x) = x

p sa([:a, :b, :c]).find { |n| n == :b }              # :b
p sb([:a, :b, :c]).detect { |n| n == :z }            # nil
p sc([1, "two", :three, 4]).find { |e| e == :three } # :three

S = Struct.new(:node)
r = su([S.new(:a), S.new(:b)]).find { |n| n.node == :b }
p r.node                                             # :b
p su([S.new(:a)]).find { |n| n.node == :zzz }        # nil
