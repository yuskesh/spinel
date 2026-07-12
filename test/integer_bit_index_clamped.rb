# Integer#[i] with an out-of-range bit index: the emitted C shift is
# clamped (sp_int_bit), so i >= 64 reads the sign fill and a negative
# index is 0 -- the literal-folded form was an undefined C shift that
# only LOOKED right on x86's masked shifts.
puts 5[100]
puts 5[64]
puts 5[63]
puts 5[-1]
puts 5[0]
puts 5[2]
puts(-1[100])
puts(-5[64])
i = 100
puts 5[i]
puts(-1[i])
