# A binary string (invalid UTF-8, e.g. a binary-mode read) counts its length in
# bytes (Ruby ASCII-8BIT semantics), matching bytesize -- not UTF-8 codepoints.
bin = [0x00, 0x80, 0xC0, 0xFF, 0x41, 0xFE].pack('C*')
puts "size=#{bin.size} bytesize=#{bin.bytesize}"
# valid UTF-8 text still counts codepoints
txt = "abc"
puts "txt size=#{txt.size} bytesize=#{txt.bytesize}"
# valid *multibyte* UTF-8: size counts codepoints while bytesize counts bytes,
# so the two must differ -- this exercises the char-count path (unlike ASCII,
# where size == bytesize would pass either way).
snow = "☃"          # snowman, one codepoint, three UTF-8 bytes
puts "snow size=#{snow.size} bytesize=#{snow.bytesize}"
acc = "café"        # "cafe" with an accent: four codepoints, five bytes
puts "acc size=#{acc.size} bytesize=#{acc.bytesize}"
mix = "aé☃z"   # ascii + 2-byte + 3-byte + ascii = 4 chars, 7 bytes
puts "mix size=#{mix.size} bytesize=#{mix.bytesize}"
# a longer binary blob
blob = ([0xAA] * 100).pack('C*')
puts "blob size=#{blob.size}"
