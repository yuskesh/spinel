def m(a)
  yield a
end
p(m([1, 2]) { |a, | a })
p(m([1, 2]) { |a, *b, c, d| [a, b, c, d] })
p(m([1, 2]) { |a=5, b=4, c=3| [a, b, c] })
p(m([1, 2]) { |a=5, b, c, d| [a, b, c, d] })
p(m([1]) { |a, b=5, c=6, d, e| [a, b, c, d, e] })
p(m([1, 2]) { |a, b=5, c=6, d, e| [a, b, c, d, e] })
p(m([1, 2, 3]) { |a, b=5, c=6, d, e| [a, b, c, d, e] })
p(m([1, 2, 3, 4]) { |a, b=5, c=6, d, e| [a, b, c, d, e] })
p(m([1, 2, 3, 4, 5]) { |a, b=5, c=6, d, e| [a, b, c, d, e] })
p(m([1, 2, 3, 4, 5, 6]) { |a, b=5, c=6, d, e| [a, b, c, d, e] })
p(m([1, 2, 3, 4, 5]) { |a, *b, c| [a, b, c] })
p(m([1, 2, 3]) { |*a, b| [a, b] })
def m2(x, y, z)
  yield x, y, z
end
p(m2(1, 2, 3) { |a, *b, c| [a, b, c] })
p(m2(1, 2, 3) { |a, b=9, c| [a, b, c] })
