# Regexp introspection: #encoding (was silently ""), #fixed_encoding?, and
# Regexp.linear_time? (both previously rejected at compile time).

# An ASCII-only regexp is US-ASCII and not fixed-encoding.
p(/abc/.encoding.name)      # "US-ASCII"
p(/abc/.encoding)           # #<Encoding:US-ASCII>
p(/abc/.fixed_encoding?)    # false
p(/a(b)c/.fixed_encoding?)  # false

# A regexp with multibyte UTF-8 source is UTF-8 and fixed-encoding.
p(/café/.encoding.name)  # "UTF-8"
p(/café/.fixed_encoding?) # true

# Regexp.linear_time?: true for backref-free patterns, false when a
# backreference (which defeats the linear matcher) is present.
p Regexp.linear_time?(/abc/)              # true
p Regexp.linear_time?(/(a|b)*c/)          # true
p Regexp.linear_time?(/(a)\1/)            # false  (numbered backref)
p Regexp.linear_time?(/(?<n>\w+)\k<n>/)   # false  (named backref)

# Backref-like syntax inside a [...] class is octal / literal, not a real
# backreference, so the pattern stays linear; a real backref after the class
# still trips (escaped brackets inside the class are handled too).
p Regexp.linear_time?(/[\1]/)             # true   (\1 is octal in a class)
p Regexp.linear_time?(/[a-z\k<x>]/)       # true   (\k literal in a class)
p Regexp.linear_time?(/[\1](a)\1/)        # false  (class \1 ignored; real \1 caught)
p Regexp.linear_time?(/[\]]\1(a)/)        # false  (escaped ] in class, then backref)

# Non-literal regexp (routed through a method) defaults to the supported
# ASCII domain.
def s(x); x; end
p s(/xyz/).encoding.name      # "US-ASCII"
p s(/xyz/).fixed_encoding?    # false
p Regexp.linear_time?(s(/xyz/))  # true
