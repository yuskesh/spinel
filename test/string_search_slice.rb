# String search/slice coverage: index-by-substring (returns the substring or
# nil), start_with? with a regex (anchored at index 0), the byte-sum checksum,
# and unpack1 (the first element of unpack). Receivers go through a method
# param so the runtime path is exercised rather than constant folding.
def s(x); x; end

# s["sub"]: the matched substring, or nil
p s("hello")["ll"]
p s("hello")["zz"]
p s("hello")["hello"]

# start_with? accepts a regex, true only when it matches at the start
p s("hello").start_with?(/he/)
p s("hello").start_with?(/ell/)
p s("hello").start_with?(/zz/)
p s("hello").start_with?("he")

# sum: 16-bit byte checksum
p s("hello").sum
p s("abc").sum

# unpack1: first element of unpack
p s("AB").unpack1("C")
p s("ABCD").unpack1("N")
