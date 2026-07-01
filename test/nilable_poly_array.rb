# A method whose value is `poly_array | nil` stays a (nullable) poly array rather
# than widening to a bare poly, so array methods still dispatch on the result.
def gen(x)
  if x > 0
    [:begin, :a, :b]
  end          # implicit nil branch makes the return nilable
end

g = gen(5)
p g                          # [:begin, :a, :b]
p g.reject { _1 == :begin }  # [:a, :b]
p g.map { |e| e.to_s }       # ["begin", "a", "b"]
p g.join("-")                # "begin-a-b"
