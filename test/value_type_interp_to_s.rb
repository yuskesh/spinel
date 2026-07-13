class Point
  def initialize(x)
    @x = x
  end
  def to_s
    "P#{@x}"
  end
end
pt = Point.new(3)
puts "point: #{pt}"
