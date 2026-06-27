# "%b" / "%B" binary conversion in String#% / format / sprintf. Previously the
# binary conversion was unsupported and emitted the literal spec (e.g. "%b").
def s(x); x; end

# --- basics ---
p("%b" % 10)        # 1010
p("%b" % 0)         # 0
p("%b" % 1)         # 1
p("%B" % 10)        # 1010
p("%b" % 255)       # 11111111

# --- width (space-padded, zero-padded, left-justified, too-narrow) ---
p("%5b" % 10)       # " 1010"
p("%8b" % 10)       # "    1010"
p("%1b" % 10)       # "1010" (width < length: no truncation)
p("%08b" % 10)      # "00001010"
p("%-8b|" % 10)     # "1010    |"

# --- flags on non-negatives ---
p("%+b" % 10)       # +1010
p("% b" % 10)       # " 1010"
p("%#b" % 10)       # 0b1010
p("%#B" % 10)       # 0B1010
p("%#b" % 0)        # 0      (# prefix omitted for zero)
p("%+08b" % 10)     # +0001010
p("% 08b" % 10)     # " 0001010"
p("%-#10b|" % 10)   # "0b1010    |"
p("%#010b" % 10)    # 0b00001010

# --- precision (minimum digits; disables 0-flag) ---
p("%.8b" % 5)       # 00000101
p("%.0b" % 0)       # ""      (zero with precision 0 is empty)
p("%.1b" % 0)       # 0
p("%.3b" % 10)      # 1010    (precision < length: no truncation)
p("%.0b" % 5)       # 101
p("%10.8b" % 5)     # "  00000101"
p("%-10.8b|" % 5)   # "00000101  |"
p("%#.8b" % 5)      # 0b00000101
p("%#.4b" % 0)      # 0000
p("%05.3b" % 0)     # "  000"

# --- negatives without a sign flag: two's-complement ".." notation ---
p("%b" % -1)        # ..1
p("%b" % -2)        # ..10
p("%b" % -5)        # ..1011
p("%.8b" % -5)      # ..111011
p("%#b" % -5)       # 0b..1011
p("%10b" % -5)      # "    ..1011"
p("%-10b|" % -5)    # "..1011    |"
p("%010b" % -5)     # ..11111011   (zero-pad fills with the sign bit 1)
p("%012b" % -5)     # ..1111111011
p("%#010b" % -5)    # 0b..111011

# --- negatives WITH a sign flag: signed magnitude ("-101"), not ".." ---
p("%+b" % -5)       # -101
p("% b" % -5)       # -101
p("%+.8b" % -5)     # -00000101
p("%+08b" % -5)     # -0000101
p("%+10b" % -5)     # "      -101"
p("%+#b" % -5)      # -0b101

# --- large precision exercises the digit buffer (this path overflowed the
#     stack before the buffers were sized to the output ceiling) ---
p("%.100b" % 5)     # 97 zeros then 101
p("%.64b" % -5)     # ".." then 1-padded body to 64 chars
p("%.128b" % 1)     # 127 zeros then 1

# --- routing: multi-arg, method-routed value, format, sprintf ---
p("%b and %b" % [5, 6])   # "101 and 110"
p("v=%b" % s(6))          # "v=110"
p(format("%08b", 42))     # 00101010
p(sprintf("%b", 255))     # 11111111
p("%b" % s(-3))           # ..101
