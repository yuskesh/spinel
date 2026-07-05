# A Struct with a custom initialize that calls super(...) must set every member
# via super, including computed (Array) args -- not leave them nil.
SW = 4
V = Struct.new(:a, :b, :top, :bottom, :minx, :maxx) do
  def initialize(a, b)
    super(a, b, Array.new(SW, 100), Array.new(SW, -1), SW, -1)
  end
  def mark(x, y1)
    top[x] = [top[x], y1].min
    bottom[x] = [bottom[x], y1].max
  end
end
v = V.new(1, 2)
puts [v.a, v.b, v.minx, v.maxx].inspect
puts v.top.inspect
puts v.bottom.inspect
v.mark(0, 50)
puts v.top.inspect
puts v.bottom.inspect
