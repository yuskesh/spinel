# Bitwise ops with a bignum receiver (a value > Integer 63-bit) -- the 64-bit
# unsigned masking idiom used by PRNGs/hashes. The receiver, the mask, and the
# result can all exceed int64, so they must use arbitrary-precision bitwise,
# not an int64 round-trip. (#1516)
MASK64 = (1 << 64) - 1
puts(1 << 64)                                  # 18446744073709551616
puts(MASK64)                                   # 18446744073709551615
a = 0x9e3779b97f4a7c16                         # > int63, a bignum
puts(a & MASK64)                               # 11400714819323198486
puts(a & 0xFFFFFFFFFFFFFFFF)                   # same, hex mask
puts(a | 0xFF)                                 # 11400714819323198719
puts(a ^ 0xFFFFFFFFFFFFFFFF)                   # 7046029254386353129
puts(a >> 4)                                   # 712544676207699905
puts(a << 4 & MASK64)                          # 2545957259850036576

# SplitMix64: every step is `(x + K) & MASK64`, computed in 64-bit unsigned.
sm = 0
out = []
5.times do
  sm = (sm + 0x9e3779b97f4a7c15) & MASK64
  z = sm
  z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & MASK64
  z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & MASK64
  out << (z ^ (z >> 31))
end
puts out.join(",")
