# Issue #848: out-of-range / unmatched backreference ($3 when
# the regex has 2 captures) returns nil, not "". Codegen now
# returns sp_re_captures[N] directly (NULL for unset) so
# downstream NULL-safe paths render it as nil.
"ab" =~ /(a)(b)/
puts "$1=#{$1.inspect}"
puts "$2=#{$2.inspect}"
puts "$3=#{$3.inspect}"
puts "$4=#{$4.inspect}"

# No match clears all captures.
"hello" =~ /xyz/
puts "$1=#{$1.inspect}"
