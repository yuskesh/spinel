# Symbol string-surface methods, lowered over the symbol's name: index/slice
# yield a substring (or nil), succ re-interns a symbol, and start_with?/
# end_with?/match? yield a bool. Receivers go through a method param so the
# symbol value reaches the runtime path rather than being folded.
def wrap(x); x; end

p wrap(:hello)[1]
p wrap(:hello)[1, 3]
p wrap(:hello)[10]
# a Range index yields a substring (inclusive, exclusive, and endless forms)
p wrap(:hello)[1..3]
p wrap(:hello)[1...3]
p wrap(:hello)[2..]
p wrap(:abc).succ
p wrap(:az).succ
p wrap(:hello).start_with?("he")
p wrap(:hello).start_with?("xx")
p wrap(:hello).end_with?("lo")
p wrap(:hello).end_with?("xx")
p wrap(:hello).match?(/ell/)
p wrap(:hello).match?(/zzz/)

# match? also works when the pattern is a non-literal regex (a local / param),
# matching over the symbol's name rather than the raw symbol value.
rx = /^he/
p wrap(:hello).match?(rx)
def sym_match(s, r); s.match?(r); end
p sym_match(:hello, /lo$/)
p sym_match(:hello, /zzz/)
