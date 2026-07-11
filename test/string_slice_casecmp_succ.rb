# CRuby contracts for three String surfaces: the [start, len] / [range]
# nil boundaries, casecmp/casecmp? answering nil for a non-String argument,
# and the alphanumeric-aware succ carry.

def sub2(s, i, n); s[i, n]; end

# in-range, boundary (start == length -> ""), past the end -> nil
p sub2("hello", 1, 3)
p sub2("hello", 5, 2)
p sub2("hello", 6, 2)

# negative starts: in range counts back; before -length -> nil
p sub2("hello", -4, 2)
p sub2("hello", -5, 1)
p sub2("hello", -6, 1)

# negative length -> nil; zero length -> ""
p sub2("hello", 1, -1)
p sub2("hello", 1, 0)

def subr(s, r); s[r]; end
p subr("hello", 3..9)
p subr("hello", 9..12)
p subr("hello", -99..2)
p subr("hello", 5..7)

# slice is the same method
def sl2(s, i, n); s.slice(i, n); end
p sl2("hello", 10, 2)
p sl2("hello", 2, 2)

def cc(a, b); a.casecmp(b); end
def ccp(a, b); a.casecmp?(b); end
p cc("abc", "ABD")
p cc("ABC", "abc")
p ccp("abc", "ABC")
p ccp("abc", "abd")
p cc("abc", :abc)
p ccp("abc", :abc)
p ccp("abc", 42)

def sc(x); x.succ; end
# digit carry across a same-class gap; alpha likewise
p sc("1.9")
p sc("1.99")
p sc("z.z")
# adjacent alnum takes the carry regardless of class
p sc("a9")
p sc("1z")
p sc("z9")
p sc("Zz9")
# a different class across a gap inserts instead of carrying
p sc("a-9")
p sc("1.z")
p sc(".9")
# no alphanumerics: the last byte increments
p sc("***")
p sc("<<koala>>")
