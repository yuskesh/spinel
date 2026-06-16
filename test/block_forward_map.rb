# A method taking a block (&blk) can forward it to an array's #map /
# #collect: each element is mapped through the caller's block and the
# results are collected. The forwarded `&blk` is a BlockArgumentNode
# with no body, so the map codegen resolves it to the active inlined
# block instead of treating it as an empty (nil-producing) block.
# Reuses the typed-map machinery, so element/result type inference
# (int, string, and int<->string conversions) all flow through.
def doubled(arr, &blk)
  arr.map(&blk)
end
p doubled([1, 2, 3]) { |x| x * 2 }

# #collect alias, result fed into a further computation.
def mapped(arr, &blk)
  arr.collect(&blk)
end
p mapped([1, 2, 3]) { |n| n * n }

# int element -> string result.
def to_strings(arr, &blk)
  arr.map(&blk)
end
p to_strings([1, 2, 3]) { |x| x.to_s }

# string element -> int result.
def lengths(arr, &blk)
  arr.map(&blk)
end
p lengths(["a", "bb", "ccc"]) { |s| s.length }

# Class instance method forwarding &blk to #map.
class Calc
  def squares(arr, &blk)
    arr.map(&blk)
  end
end
p Calc.new.squares([1, 2, 3]) { |n| n * n }

# Method does work before forwarding; the forward still resolves.
def announced(arr, &blk)
  puts "mapping"
  arr.map(&blk)
end
p announced([2, 4, 6]) { |x| x / 2 }

# Anonymous `&` forward (Ruby 3.1+): the unnamed block param forwards
# through the matching anonymous `&` at the #map call.
def tripled(arr, &)
  arr.map(&)
end
p tripled([1, 2, 3]) { |x| x * 3 }
