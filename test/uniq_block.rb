# Array#uniq / uniq! with a block dedupe by the block's return value, keeping
# the first element of each key group and preserving the receiver's element type.
# Receivers are routed through per-type identity methods to defeat constant
# folding and exercise the real typed runtime path.
def ai(x); x; end
def af(x); x; end
def ap(x); x; end

# Integer array.
p ai([1, 2, 3, 4]).uniq { |n| n % 2 }          # [1, 2]
p [1, 2, 3, 4, 5, 6].uniq { |n| n % 3 }        # [1, 2, 3]
p ai([1, 2, 3, 4]).uniq { |n| n % 2 }.sum      # 3  (result stays an Int array)

# String array.
p %w[foo bar baz qux].uniq { |w| w[0] }        # ["foo", "bar", "qux"]

# Float array.
p af([1.5, 2.5, 3.5, 4.5]).uniq { |x| x.to_i % 2 }   # [1.5, 2.5]

# Heterogeneous (poly) array.
p ap([1, "a", 2, "b"]).uniq { |x| x.class }    # [1, "a"]

# Empty and all-unique-key cases.
p [].uniq { |n| n }                            # []
p [5, 6, 7].uniq { |n| n }                     # [5, 6, 7]

# uniq! mutates in place.
a = [1, 1, 2, 3, 3]
a.uniq! { |n| n }
p a                                            # [1, 2, 3]
