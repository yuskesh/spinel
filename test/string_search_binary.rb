# String#include?/#index/#rindex must be byte-oriented and NUL-transparent:
# spinel Strings can carry embedded NULs (pack, socket reads), so a strstr-
# backed search that stops at the first NUL diverges from CRuby (issue #1778).
z = [0].pack("C*")
s = "user" + z + "app" + z + "tail"
puts s.include?("app" + z)   # needle contains a NUL
puts s.include?("tail")      # plain needle after the haystack's first NUL
puts s.index("tail")
puts s.index("app")
puts s.rindex("app")
# ordinary (NUL-free) searches are unchanged
puts "hello world".include?("lo wo")
puts "hello world hello".index("hello", 3)
puts "hello world hello".rindex("hello")
puts "café テスト".index("テスト")
