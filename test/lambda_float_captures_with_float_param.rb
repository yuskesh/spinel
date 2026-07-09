# A multi-statement lambda/proc may capture two or more non-Integer (Float,
# String, ...) locals even when it also takes a Float parameter. Captures ride
# the capture struct as real fields; a first-class proc's .call passes Float
# args through the boxed side-channel, so there is no truncation conflict.

t = 0.5
g = 0.25
f = lambda do |i, alpha|
  x = (i + 0.5) * 2.0
  s = (6.0 + (30.0 * g)) * (t + i)
  [x, s, alpha]
end
3.times { |i| p f.call(i, 0.7) }

a = 1.5
b = 2.5
c = 3.5
multi = lambda do |x, y|
  p1 = x + a
  p2 = y * b
  p3 = c - x
  [p1, p2, p3]
end
p multi.call(0.5, 2.0)

name = "hi"
scale = 10.0
interp = ->(v) { "#{name}:#{v * scale}" }
p interp.call(1.5)
