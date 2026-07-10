# The capacity: and encoding keyword arguments to String.new are hints that do
# not change the value -- the content is the leading positional argument. Spinel
# previously dropped the content whenever a keyword argument was present,
# returning "".
def with_cap; String.new("x", capacity: 100); end
p with_cap                     # "x"

def only_cap; String.new(capacity: 50); end
p only_cap                     # ""

def plain; String.new("y"); end
p plain                        # "y"

def empty; String.new; end
p empty                        # ""

# The result is a mutable copy (a literal passed as content is not aliased).
def mutable
  s = String.new("ab", capacity: 8)
  s << "c"
  s
end
p mutable                      # "abc"

# Content from a variable, with a capacity hint.
def from_var(seed); String.new(seed, capacity: 32); end
p from_var("hello")            # "hello"
