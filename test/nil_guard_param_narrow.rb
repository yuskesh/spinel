def up(s)
  return "nil" if s.nil?
  s.upcase
end
puts up("hi")
puts up(nil)

def fmt(t)
  return "null" if t.nil?
  t.strftime("%Y")
end
puts fmt(Time.at(0).utc)
puts fmt(nil)

def inc(x)
  return -1 if x.nil?
  x + 1
end
p inc(41)
p inc(nil)

def two(a, b)
  return "a-nil" if a.nil?
  return "b-nil" if b.nil?
  a + b.upcase
end
puts two(nil, "x")
puts two("x", nil)
puts two("x-", "y")

def blocky(s)
  return 0 if s.nil?
  n = 0
  2.times { n += s.length }
  n
end
p blocky("abc")
p blocky(nil)

def reassigned(s)
  return "n" if s.nil?
  s = nil if s == "z"
  s.to_s
end
puts reassigned("a")
puts reassigned("z")
puts reassigned(nil)
