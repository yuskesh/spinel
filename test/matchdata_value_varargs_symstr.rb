# MatchData as a first-class value (p/inspect/.class); String#index with a
# variable Regexp and String#[] with a variable Range; Symbol's string-
# flavored methods (regex match/slice, comparisons) through the name text.
p("hello".match(/xyz/))
p("hello".match(/l/).class)
m = "hello".match(/l(l)o/)
p m[0]
p m[1]
p m
n = "2024-01".match(/(?<y>\d+)-(?<m>\d+)/)
p n
a = "abcabc"; r = /b/; p(a.index(r))
s = "hello"; rg = 1..3; p(s[rg])
p(:hello.match(/l(l)o/)[1])
p(:hello =~ /llo/)
p(:hello.start_with?(/he/))
p(:hello[/l+/])
p(:hello.between?(:aaa, :zzz))
p(:abc <=> :abd)
p(:a < :b)
