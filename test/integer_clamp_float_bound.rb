# Integer#clamp returns the applied operand unchanged, so a Float bound that
# clamps yields a Float while an in-range Integer receiver stays an Integer.
# Receiver and bounds route through a method param so the runtime helper is
# exercised rather than a compile-time fold.
def s(x); x; end

# clamped to a Float bound -> Float
p s(5).clamp(1.0, 3.0)
p s(0).clamp(1.0, 10.0)
# in range with Float bounds -> Integer receiver kept
p s(5).clamp(1.0, 10.0)
# one int bound, one float bound
p s(5).clamp(1, 3.0)
p s(2).clamp(1, 3.5)
p s(7).clamp(1, 3.5)
# pure-int bounds still Integer
p s(5).clamp(1, 10)
p s(-4).clamp(0, 9)
# classes
p s(5).clamp(1.0, 3.0).class
p s(5).clamp(1.0, 10.0).class
p s(2).clamp(1, 3.5).class

# exceptional: disordered / NaN bounds raise ArgumentError with CRuby messages
def show(x, lo, hi)
  p x.clamp(lo, hi)
rescue => e
  puts "#{e.class}: #{e.message}"
end
nan = 0.0 / 0.0
show(s(5), 10, 2)
show(s(5), nan, 2.0)
show(s(5), 1.0, nan)
show(s(5), 1, nan)
show(s(5), nan, 2)
