# Imported mruby-regexp e246b2c05: {0,m} matched one copy too many. The
# quantifier compiler emitted the atom once up front and counted it as the
# first mandatory copy, which is wrong when the lower bound is zero. {0,m}/{0,}
# now compile as {1,m}/{1,} wrapped in an optional split, and {0} drops the copy.

p(/a{0,2}/.match("aaaa")[0])      # "aa"
p(/\d{0,3}/.match("12345")[0])    # "123"
p(/a{0,2}b/.match("aaab")[0])     # "aab"
p("aaaa".scan(/a{0,2}/))          # ["aa", "aa", ""]
p(/x{0,0}/.match("xxx")[0])       # ""
p(/a{0}/.match("aaa")[0])         # ""  ({0} matches zero)
p(/a{0,}/.match("aaaa")[0])       # "aaaa"  ({0,} == a*)
p(/a{0,}b/.match("b")[0])         # "b"
p(/a{0,}/.match("bbb")[0])        # ""  (zero a's)

# Non-zero lower bounds and exact counts unchanged.
p(/a{1,3}/.match("aaaaa")[0])     # "aaa"
p(/a{2,4}/.match("aaaaaa")[0])    # "aaaa"
p(/a{3}/.match("aaaaa")[0])       # "aaa"
p(/a{2,}/.match("aaaa")[0])       # "aaaa"
p(/\d{0,2}-\d{0,2}/.match("12-34")[0]) # "12-34"

# Non-greedy zero-lower.
p(/a{0,3}?/.match("aaa")[0])      # ""
p("aaa".match(/a{0,3}?b?/)[0])    # ""
