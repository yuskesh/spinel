# An empty Hash.new(default) is a real hash: size/inspect/p work before any
# key is inserted (it used to raise "uninitialized constant Hash"), and the
# default is served for missing keys.
h = Hash.new(0)
puts h.size
puts h.inspect
puts h[:missing]
h[:a] += 1
puts h.inspect
p Hash.new(0)
g = Hash.new("d")
puts g["nope"]
puts g.size
e = Hash.new
puts e.size
