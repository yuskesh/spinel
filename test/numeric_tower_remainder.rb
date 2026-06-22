# Numeric tower remainder: Integer#[start, len] (the len-bit field starting at
# bit `start`) and Math.lgamma (which returns [log(|gamma(x)|), sign]). The
# integer receiver goes through a method param to exercise the runtime path.
def i(x); x; end

# bit-range field extraction
p i(0b1011010)[1, 3]
p i(255)[0, 4]
p i(255)[4, 4]
p i(0b110100)[2, 4]
p i(42)[2]

# Out-of-range start/len must not trigger an undefined shift: a len at/above
# the word width keeps every shifted bit, a non-positive len or a start past
# the width behaves as CRuby's two's-complement model does.
def br(n, s, l); n[s, l]; end
p br(255, 2, 100)
p br(5, 2, -1)
p br(5, -1, 3)
p br(255, 0, -5)
p br(-1, 60, 8)
p br(255, 100, 4)
p br(1, 64, 1)
p br(7, 0, 0)

# Math.lgamma -> [value, sign]. The value is computed in-tree (Stirling's
# series + the Gamma recurrence) and ultimately depends on libm log()/sin(),
# whose last ULP is not portable across implementations, so assert it within a
# tight tolerance rather than pinning the exact bits. The exact points
# (lgamma(1) = lgamma(2) = 0) and the sign are bit-stable and checked directly.
g5 = Math.lgamma(5)                                  # ~ log(24)
p(g5[0] > 3.1780538303 && g5[0] < 3.1780538304)
p g5[1]
gh = Math.lgamma(0.5)                                # ~ log(sqrt(pi))
p(gh[0] > 0.5723649429 && gh[0] < 0.5723649430)
p gh[1]
p Math.lgamma(1)
p Math.lgamma(2)
