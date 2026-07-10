# Under the default --int-overflow=raise, every integer operator raises on
# 64-bit overflow the same way, whether written in binary or op-assign form.
# Regression: `x *= y` (and +=, <<=, **=) bypassed the checked helpers and
# silently wrapped; `1 << 63` landed on the tagged-nil sentinel and printed
# nil; a runtime `a ** 64` went through pow(double) and printed garbage.
def try
  yield
rescue RangeError => e
  puts "RangeError: #{e.message}"
end

a = 999_999_999_999
try { a *= 999_999_999_999 }
b = 10
try { b += 9_223_372_036_854_775_800 }

c = 1
try { puts c << 63 }
try { puts c << 64 }
puts c << 62
d = 1
try { d <<= 63 }
puts d

pw = 2
try { puts pw ** 64 }
puts pw ** 62
f = 5
f **= 3
puts f

# op-assign division adopts Ruby semantics with the checked helpers
g = -7
g /= 2
puts g
h = -7
h %= 3
puts h

# ivar op-assign takes the same checked path
class Acc
  attr_reader :n
  def initialize = (@n = 999_999_999_999)
  def blow = (@n *= 999_999_999_999)
end
acc = Acc.new
begin
  acc.blow
rescue RangeError => e
  puts "ivar RangeError: #{e.message}"
end
