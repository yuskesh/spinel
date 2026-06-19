# String#<< must preserve embedded NUL bytes: a mutable string's payload buffer
# is length-tracked (a header on the 0xfd buffer), not NUL-terminated, so
# appending content that begins with (or contains) 0x00 keeps every byte.
# (matz/spinel#1479)

a = "a"; a << "\x00"
puts a.length                 # 2

b = "a"; b << 0.chr
puts b.length                 # 2

c = String.new
c << 126.chr; c << 0.chr; c << 200.chr
puts c.length                 # 3
puts c.bytes.inspect          # [126, 0, 200]

# Operand carrying an interior NUL.
d = "x"
d << "\x7e\x00\xc8"
puts d.bytesize               # 4
puts d.bytes.inspect          # [120, 126, 0, 200]

# A WebSocket extended-length header (0x7e 0x00 <lo>) survives the emit path.
payload = "y" * 130
frame = String.new
frame << 0x81.chr << 0x7e.chr << ((payload.bytesize >> 8) & 0xFF).chr << (payload.bytesize & 0xFF).chr
frame << payload
puts frame.bytesize           # 134
puts frame.getbyte(2)         # 0
puts frame.getbyte(3)         # 130

# Mutating via <<, then reading the escaped value (length over a const char*).
e = "ab"
e << 0.chr
e << "cd"
puts e.length                 # 5
puts (e + "!").bytesize       # 6

# String#+ (already binary-safe) still agrees.
puts ("a" + "\x00").length    # 2
