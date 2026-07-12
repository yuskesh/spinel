class Nums
  include Enumerable
  def each
    yield 3
    yield 1
    yield 2
  end
end
n = Nums.new
p n.to_a
p n.sort
p n.map { |x| x * 10 }
p n.include?(2)
