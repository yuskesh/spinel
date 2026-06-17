class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def sum
    @x + @y
  end
end

r = Ractor.new do
  p = Ractor.receive
  Ractor.yield(p.sum)
end
r << Point.new(10, 32)
puts r.take

# nested object + as spawn arg
class Box
  def initialize(label, point)
    @label = label
    @point = point
  end
  def describe
    @label + ":" + @point.sum.to_s
  end
end
r2 = Ractor.new(Box.new("pt", Point.new(3, 4))) do |b|
  Ractor.yield(b.describe)
end
puts r2.take
