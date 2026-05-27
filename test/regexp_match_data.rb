# Issue #867: String#match and Regexp#match return a MatchData-like
# object instead of falling through to unresolved-call emit-0.
md1 = "hello world".match(/\w+/)
puts md1[0]

md2 = /(\w+) (\w+)/.match("hello world")
puts md2[0]
puts md2[1]
puts md2[2]

md3 = "a".match(/(a)(b)?/)
puts md3[2].nil? ? "nil capture" : "bad capture"
puts "abc".match(/z/).nil? ? "nil match" : "bad match"

md4 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".match(/(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)(a)/)
puts md4.length
puts md4[31]
