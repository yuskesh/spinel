# reduce/inject with an array-accumulator seed, and sum with a string init
# and block.
def f(xs); xs.reduce([]) { |a, x| a << x }; end
def g(a); a.reduce([]) { |acc, x| acc << x }; end
def h(xs); xs.inject([]) { |a, x| a << (x * 2) }; end

p f([1, 2])            # [1, 2]
p f([])                # []
p g([1, "x"])          # [1, "x"]
p h([3, 4])            # [6, 8]
p [1, 2, 3].reduce(0) { |a, x| a + x }      # 6 (scalar seeds unchanged)
p [1, 2].reduce(0.5) { |a, x| a + x }       # 3.5
a = [1, 2, 3]
p a.sum("") { |x| x.to_s }                  # "123"
p ["a", "b"].sum("> ") { |x| x }            # "> ab"
p [1, 2].sum(10) { |x| x * 2 }              # 16 (numeric unchanged)
