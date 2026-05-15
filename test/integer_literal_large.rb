# Integer literals larger than uint32 must keep their full mrb_int value.
#
# Prism stores those literals in 32-bit limbs. The parser must combine the
# limbs instead of reading only the embedded small-integer field, otherwise
# 4_294_967_296 (2^32) serializes as 0 and modulo expressions divide by zero.

mod = 4_294_967_296

puts mod
puts 16_777_621 % mod

hash = 2_166_136_261
data = "spinel"
i = 0
while i < data.length
  hash = (hash * 16_777_619 + data.getbyte(i)) % mod
  i += 1
end

puts hash
