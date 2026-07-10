# A string param mutated via `<<` is a byref out-param: the append lands in
# the caller's variable, like CRuby's shared-object semantics (the classic
# pass-an-output-buffer serializer pattern).
def append(s)
  s << "world"
end
str = "hello".dup
append(str)
puts str

# transitive: a wrapper that passes its param on into a byref slot
def inner(s)
  s << "!"
end
def outer(s)
  inner(s)
end
a = "x".dup
outer(a)
puts a

# the mutator's return value is the receiver, usable alongside the mutation
def bang(s)
  s << "?"
end
b = "y".dup
r = bang(b)
puts b
puts r

# only the mutated param rides byref; its sibling stays a plain value
def tag(buf, label)
  buf << "["
  buf << label
  buf << "]"
end
t = "".dup
tag(t, "hi")
puts t

# a non-lvalue argument still compiles (CRuby's mutation is equally
# invisible to the caller there)
bang("zzz".dup)
puts "lit ok"

# a plain rebind keeps value semantics: CRuby `s = ...` rebinds the local,
# invisible to the caller, so the param must NOT ride byref
def rebind(s)
  s = "gone"
  s << "!"
end
u = "keep".dup
rebind(u)
puts u

# repeated appends through a call inside a block (build-a-buffer loop)
def ser(buf, n)
  buf << n.to_s
  buf << ","
end
o = "".dup
3.times { |i| ser(o, i) }
puts o

# a caller variable that is also captured by a proc (celled) passes its cell
w = "p".dup
f = proc { w << "c" }
bang(w)
f.call
puts w

# a frozen argument raises FrozenError from inside the callee, like CRuby
frozen = "locked".freeze
begin
  bang(frozen)
rescue FrozenError
  puts "FrozenError"
end
