# Object#== with no user-defined == is identity comparison: an instance is
# equal only to itself, and two distinct instances are never equal. Receivers
# go through method params so the compare hits the runtime object path (and so
# the class stays pointer-backed rather than a by-value type).
class Widget
  def initialize(n)
    @n = n
  end

  def n
    @n
  end
end

def same?(a, b)
  a == b
end

def diff?(a, b)
  a != b
end

w = Widget.new(1)
x = Widget.new(1)

puts same?(w, w)   # same object -> true
puts same?(w, x)   # distinct objects, same contents -> false (identity)
puts diff?(w, x)   # true
puts diff?(w, w)   # false
puts(w.n == x.n)   # ivar values compared, not identity -> true
