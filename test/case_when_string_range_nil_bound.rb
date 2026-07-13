# A nil endpoint of a string range (here an unset String ivar, which reads as a
# NULL slot) is an OPEN bound under Range#=== / case-when, exactly like CRuby:
# `(nil.."m") === "c"` is true. The endpoint must not be dereferenced by strcmp,
# and must widen its side rather than fail the match.
class Bounds
  def initialize(set_lo, set_hi)
    @lo = "d" if set_lo
    @hi = "m" if set_hi
  end
  def classify(s)
    case s
    when @lo..@hi then "in"
    else "out"
    end
  end
end

p Bounds.new(false, true).classify("c")   # open lower, "c" <= "m"  -> in
p Bounds.new(false, true).classify("z")   # open lower, "z" >  "m"  -> out
p Bounds.new(true, false).classify("f")   # "f" >= "d", open upper  -> in
p Bounds.new(true, false).classify("a")   # "a" <  "d"              -> out
p Bounds.new(true, true).classify("g")    # "d" <= "g" <= "m"       -> in
p Bounds.new(false, false).classify("x")  # both open               -> in
