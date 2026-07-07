# Integer#bit_length on a Bignum (a value larger than int64): the parser lexes
# the literal into a bigint, and bit_length answers the number of bits, an int.

p 9223372036854775808.bit_length     # 64  (2**63, the first value past int64)
p 18446744073709551616.bit_length    # 65  (2**64)
p (2 ** 64).bit_length               # 65  (computed bignum)
p (2 ** 100).bit_length              # 101
p (10 ** 30).bit_length              # 100

# through a poly-returning method, so the bigint flows across a call boundary
def id(x) = x
p id(2 ** 70).bit_length             # 71
