# Splat-rest and trailing post-required parameters on proc literals: CRuby's
# distribution fills leading requireds from the front, posts from the back,
# the (possibly empty) remainder is the rest, and missing posts bind nil.
p1 = proc do |*a, b|
  [a, b]
end
p p1.call(1, 2, 3)
p p1.call(1)
p p1.call

p2 = proc do |x, *m, y|
  [x, m, y]
end
p p2.call(1, 2, 3, 4, 5)
p p2.call(1, 2)

# lambda: rest lifts the max, requireds+posts stay mandatory
l = lambda do |x, *m, y|
  [x, m, y]
end
p l.call(1, 9)
p l.call(1, 2, 3, 9)
begin
  l.call(1)
rescue ArgumentError
  puts "arity enforced"
end
