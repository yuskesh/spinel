# a stays a poly array (heterogeneous elements) through the method param.
def probe(a)
  [a.reverse, a.uniq, a.first(2), a.last(2), a.first(0), a.last(10)]
end

r = probe(["x", 1, :y, "x", 2, 1])
r.each { |x| p x }

# edge: first/last beyond length, and negative -> ArgumentError
def neg_first(a) = a.first(-1)
begin
  neg_first([1, "a", :b])
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end
