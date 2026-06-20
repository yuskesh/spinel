# A literal integer power whose result exceeds int64 is a Bignum in every
# overflow mode (CRuby). A power that fits stays a plain Integer.
puts 10 ** 30
puts 2 ** 70
puts 2 ** 64
puts 3 ** 40
puts 2 ** 10
puts 7 ** 2
y = 10 ** 25
puts y + 1
puts y * 2
