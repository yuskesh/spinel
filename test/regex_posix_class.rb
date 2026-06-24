# POSIX bracket classes [[:name:]] inside char classes (W9 / B49).
# Previously parsed as the literal set of characters in ":name:", so
# they never matched. Now they compile to the corresponding ASCII ranges.

# scan over each of the required class names
p "Hi 12".scan(/[[:alpha:]]+/)
p "a1 b2".scan(/[[:digit:]]+/)
p "ab 12 CD".scan(/[[:alnum:]]+/)
p "a\tb c\nd".scan(/[[:space:]]+/)
p "Hello, World".scan(/[[:upper:]]/)
p "Hello, World".scan(/[[:lower:]]/)

# negated class: [^[:digit:]]
p "a1b2c3".scan(/[^[:digit:]]+/)

# combined with literals and ranges in the same bracket expression
p "a1_b2".scan(/[[:alpha:]_]+/)

# plain single match (not scan)
p(/[[:alpha:]]+/.match("  word42")[0])
p("42abc".match(/[[:alpha:]]+/)[0])

# Non-constant subject path: route the subject string through a method
# parameter so the call is not constant-folded at compile time, forcing
# the runtime regex engine to evaluate the POSIX class. Both scan and a
# single match are exercised.
def scan_class(str) = str.scan(/[[:alpha:]]+/)
def match_class(str) = str.match(/[[:digit:]]+/)[0]
def scan_negated(str) = str.scan(/[^[:digit:]]+/)
p scan_class("Hi 12")
p match_class("a1 b2")
p scan_negated("a1b2c3")

# In-class negation [:^name:] (distinct from enclosing [^...]).
p "a1b2c3 ".scan(/[[:^digit:]]/)
p "Hello9".scan(/[[:^alpha:]]/)
p "a1_ X".scan(/[[:^word:]]/)

# [:ascii:] matches the ASCII range 0x00-0x7f.
p "aZ9!".scan(/[[:ascii:]]/)

# An unrecognized POSIX class name is a RegexpError, matching CRuby. The
# error is only catchable through the dynamic (Regexp.new) path; a bad
# literal aborts at load time like any other invalid regex literal.
begin
  Regexp.new("[[:bogus:]]")
  puts "no error"
rescue => e
  puts "#{e.class}: #{e.message}"
end
