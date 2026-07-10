# String#oct is a lenient Integer(str, 8)-style parse: leading whitespace, an
# optional sign, an optional base prefix (0x/0b/0o/0d or a leading 0 = octal),
# and single underscore separators between digits. Spinel previously mishandled
# a sign before a base prefix, dropped underscore separators, and ignored the 0d
# prefix.
def o(x); x.oct; end

p o("777")        # 511
p o("17")         # 15
p o("-0b1010")    # -10  (sign before base prefix)
p o("+17")        # 15
p o("1_0")        # 8    (underscore separator, octal)
p o("0x1f")       # 31
p o("0o17")       # 15
p o("0b11")       # 3
p o("0d99")       # 99   (0d = decimal prefix)
p o("  0b11")     # 3    (leading whitespace)
p o("0xff_ff")    # 65535
p o("0b1_1")      # 3

# Lenient stops and invalid forms return what was parsed (or 0).
p o("88")         # 0    (8 is not an octal digit)
p o("17x")        # 15   (stops at 'x')
p o("1_")         # 1    (trailing underscore)
p o("1__0")       # 1    (doubled underscore stops)
p o("_1")         # 0    (leading underscore)
p o("0b_1")       # 0    (underscore right after prefix)
p o("")           # 0
