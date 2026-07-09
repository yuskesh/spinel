# Inline option groups, imported from mruby-regexp: the toggle form (?i)
# applies to the rest of the enclosing group; the scoped form (?i:...) applies
# only to its body; negation (?-i) switches off. m is DOTALL (Ruby /m).

# scoped ignorecase
p(/(?i:abc)/ =~ "xABCy")
p(/(?i:abc)def/ =~ "ABCdef")
p(/(?i:abc)def/ =~ "ABCDEF")     # def outside the scope stays sensitive
# toggle: rest of the pattern
p(/a(?i)bc/ =~ "aBC")
p(/a(?i)bc/ =~ "Abc")            # before the toggle stays sensitive
# toggle inside a group does not leak past its ')'
p(/(a(?i)b)c/ =~ "aBc")
p(/(a(?i)b)c/ =~ "aBC")
# negation
p(/(?i:a(?-i:b)c)/ =~ "AbC")
p(/(?i:a(?-i:b)c)/ =~ "ABC")
# scoped dotall
p(/(?m:a.c)/ =~ "a\nc")
p(/a.c/ =~ "a\nc")
# combined and toggled off
p(/(?im:a.b)/ =~ "A\nB")
p(/(?i-m:a.b)/ =~ "A\nB")
p(/(?i-m:axb)/ =~ "AxB")

# Inline x: accepted when it is a no-op (no significant whitespace in its
# scope, e.g. a decorative (?x:foo)); a scope the /x strip would actually
# change raises loudly instead of silently diverging from CRuby.
p(/(?x:foo)/ =~ "foobar")
begin
  Regexp.new("(?x:a b)")
  puts "ws-x: compiled"
rescue RegexpError
  puts "ws-x: RegexpError"
end

# Invalid patterns raise RegexpError instead of hanging. Deliberate
# divergences from CRuby, which are loud (raise), not silent: CRuby merely
# warns on the redundant `a***`, and supports the absence operator (?~...)
# and conditionals; spinel rejects them at compile.
["*", "+", "?", "a***", "(?~foo)", "(?(a)b)", "(?q)a", "(?x:ab)"].each do |src|
  begin
    Regexp.new(src)
    puts "#{src}: compiled"
  rescue RegexpError
    puts "#{src}: RegexpError"
  end
end

# Truncated UTF-8 at the end of the subject must not disturb matching (and,
# under ASAN, must not read past the buffer). CRuby raises ArgumentError on
# an invalid-encoding subject; spinel matches over the bytes.
s = "abc\xE3"
p(/c/ =~ s)
p(/z/ =~ s)
# truncated multi-byte leader at the end of a character class
r = Regexp.new("[a\xE3]")
p(r =~ "za")

# a multi-member non-ASCII char class (an ascending codepoint run exercises
# the contiguous-range merge in class_add_range)
r2 = Regexp.new("[あいうえお]+")
p(r2 =~ "abcいうえxyz")
p $~[0]
p(r2 =~ "abcxyz")
