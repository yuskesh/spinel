# String#gsub / #sub expand `\k<name>` in the replacement to the named capture
# group (empty when the group did not participate). An unknown or empty name
# raises IndexError, mirroring md[:name]. `\k'name'` is regex-only and stays
# literal. The subject string is routed through a method param so the
# substitution runs at runtime rather than being constant-folded.
def s(x); x; end

# single named backref
puts s("foobar").gsub(/(?<x>o+)/, "[\\k<x>]")
# two named groups, swapped
puts s("abcd").gsub(/(?<a>a)(?<b>b)/, "\\k<b>\\k<a>")
# \k<name> mixed with \& (whole match)
puts s("ab").gsub(/(?<a>a)b/, "\\k<a>-\\&")
# optional group that participated
p s("xyz").gsub(/(?<a>x)(?<b>y)?/, "[\\k<b>]")
# optional group that did NOT participate -> empty expansion
p s("xz").gsub(/(?<a>x)(?<b>y)?/, "[\\k<b>]")
# single sub
puts s("foobar").sub(/(?<x>o+)/, "[\\k<x>]")
# \k'name' is regex-only syntax: left literal in a replacement
puts s("ab").gsub(/(?<g>a)b/, "\\k'g'")

# unknown name raises IndexError with the exact CRuby message
begin
  s("abc").gsub(/(?<x>a)/, "\\k<y>")
rescue IndexError => e
  puts "#{e.class}: #{e.message}"
end
# empty name likewise raises IndexError
begin
  s("abc").gsub(/(?<x>a)/, "\\k<>")
rescue IndexError => e
  puts "#{e.class}: #{e.message}"
end
