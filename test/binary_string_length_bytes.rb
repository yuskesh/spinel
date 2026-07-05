# A binary string (invalid UTF-8, e.g. a binary-mode read) counts its length in
# bytes (Ruby ASCII-8BIT semantics), matching bytesize -- not UTF-8 codepoints.
bin = [0x00, 0x80, 0xC0, 0xFF, 0x41, 0xFE].pack('C*')
puts "size=#{bin.size} bytesize=#{bin.bytesize}"
# valid UTF-8 text still counts codepoints
txt = "abc"
puts "txt size=#{txt.size} bytesize=#{txt.bytesize}"
# a longer binary blob
blob = ([0xAA] * 100).pack('C*')
puts "blob size=#{blob.size}"
