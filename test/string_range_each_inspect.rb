# String-endpoint ranges have no int sp_Range representation. Iterating one
# with a block (materialized as a succ-sequence) and inspecting a literal now
# work instead of miscompiling. (Accumulating the yielded strings into an array
# declared outside the block still needs wider element inference -- separate.)

# each with a block
letters = ""
("a".."e").each { |ch| letters += ch }
puts letters

# exclusive range each
("a"..."d").each { |ch| print ch }
puts

# multi-char succ
seq = ""
("aa".."ac").each { |s| seq += s + " " }
puts seq

# counting
n = 0
("a".."z").each { |_| n += 1 }
puts n

# inspect of a string-range literal (p uses #inspect: endpoints are quoted)
p ("a".."c")
p ("a"..."e")
p ("aa".."ac")

# numeric ranges are unaffected
total = 0
(1..5).each { |i| total += i }
puts total
p (1..5)
p (1...5)
