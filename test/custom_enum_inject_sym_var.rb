class Nums
  include Enumerable
  def initialize(*xs)
    @xs = xs
  end
  def each
    @xs.each { |x| yield x }
  end
end
s = :+
p Nums.new(3, 1, 2).inject(s)
p Nums.new(3, 1, 2).reduce(s)
