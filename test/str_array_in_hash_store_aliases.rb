# A String stored into a String Array that a Hash also holds lands in that
# Array: the store does not widen it into a copy (a String local is a handle).
def put(h, body)
  h[0] = Array.new(3, "") unless h.key?(0)
  pieces = h[0]
  pieces[1] = body
  puts pieces.map { |x| x.bytesize }.join(",")
  puts h[0].map { |x| x.bytesize }.join(",")
end
h = {}
put(h, "ab".byteslice(0, 2))
put(h, "cd")
a = Array.new(3, "")
a[1] = "xyz"
puts a.map { |x| x.bytesize }.join(",")
