# Calling a proc through a poly slot preserves the boxed result class: an
# escaping lambda whose body is a container literal over a captured variable
# returns the container, not a truncated scalar.
def make
  x = 5
  [-> { [x] }]
end
c = make[0]
p c.call

def make_h
  y = 7
  [-> { { v: y } }]
end
p make_h[0].call

def make_nested
  z = 3
  [-> { [[z], z + 1] }]
end
p make_nested[0].call

def make_str
  s = "cap"
  [-> { s + "!" }]
end
p make_str[0].call

def make_scalar
  n = 41
  [-> { n + 1 }]
end
p make_scalar[0].call

procs = [-> { 1 }, -> { "two" }, -> { [3] }]
p procs.map { |pr| pr.call }
