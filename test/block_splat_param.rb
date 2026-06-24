# A trailing rest block parameter (`|*a|`) collects the values yielded past the
# required parameters into an array (previously it was left nil/unbound).
def m3
  yield 1, 2, 3
end
m3 { |*a| p a }
m3 { |x, *rest| p x; p rest }

def one
  yield 5
end
one { |*a| p a }

def none
  yield
end
none { |*a| p a }

def pair
  yield "x", "y"
end
pair { |first, *others| p first; p others }

def mixed
  yield 1, "two", :three
end
mixed { |*all| p all }

# The yielded value is used (expression position): the rest array is bound and
# the block's value flows back out.
def collect
  yield 1, 2, 3
end
total = collect { |*a| a.sum }
p total
labeled = collect { |x, *rest| "#{x}:#{rest.inspect}" }
p labeled

# The bound rest is a real, usable Array (sum / inspect / map+join all work).
def emit3
  yield "p", "q", "r"
end
joined = emit3 { |*xs| xs.map { |x| x + "!" }.join("-") }
p joined
