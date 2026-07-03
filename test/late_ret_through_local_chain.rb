# A method that returns another method's value through a local (`a = nxt; a`)
# was inferred as void when the callee's own return -- also flowing through a
# local (`r = ...; r`) -- only settled in the post-fixpoint write-type re-run.
# The re-run typed the locals but never re-derived the returns, so the wrapper
# stayed at UNKNOWN, emitted as a void C function, and its value was silently
# dropped (callers read nil / 0.0). The `<<` on the ivar is what delays the
# ivar's settling past the main fixpoint. Issue #1670.

class R
  def initialize(s)
    @s0 = s
    @s1 = s + 1
  end

  def nxt
    t = (@s1 << 9) & 0xFFFFFFFF
    @s0 = @s0 ^ @s1
    @s1 = @s1 ^ t
    r = (@s0 + @s1) & 0xFFFFFFFF
    r
  end

  # int result through a local
  def uni_int
    a = nxt
    a
  end

  # float result computed from locals fed by two chained calls
  def uni_float
    a = nxt >> 5
    b = nxt >> 6
    (a * 67108864.0 + b) / 9007199254740992.0
  end
end

r1 = R.new(12345)
x = r1.uni_int
puts x                            # 6308925

r2 = R.new(12345)
puts format("%.10f", r2.uni_float)  # 0.0014689099

# Result used directly as an argument (was a compile error: "unsupported
# puts argument" once the wrapper's return degraded to void).
puts R.new(777).uni_int           # 399117
