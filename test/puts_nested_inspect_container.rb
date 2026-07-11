# puts flattens nested arrays recursively; a custom #inspect is honored for
# elements inside containers (arrays, hashes), same as when inspected directly.
puts [1, [2, [3]]]
puts [[1, 2], [3, 4]]
puts [1, [2, [3, [4, nil, [5]]]]]

class Point127
  def inspect = "#<Point>"
end
p [Point127.new]
p Point127.new
p({ a: Point127.new })
