# String#each_line with a block returns the receiver (self), per CRuby.
# The statement form already worked; the expression form (capturing the
# return value) failed to resolve -- each_line was missing from the
# string-block-method-returns-self arm that each_char / each_byte use.

s = "a\nb\nc"

# Expression form: capture the return and confirm it is the string back
r = s.each_line { |line| puts line.inspect }
puts r.class
puts r            # the whole string, unchanged
puts r.length
puts (r == s)

# Works after a conditional assignment (the issue's described trigger)
x = (1 > 0) ? "p\nq\nr" : "z"
r2 = x.each_line { |l| puts l.inspect }
puts r2.class

# chomp: keyword, expression form
r3 = "1\n2\n3\n".each_line(chomp: true) { |l| puts l }
puts r3.length

# Statement form still works
"x\ny".each_line { |l| puts l.inspect }
