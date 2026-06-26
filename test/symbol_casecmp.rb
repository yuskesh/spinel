# Symbol#casecmp and Symbol#casecmp? compare two symbols' names case-folded.
# casecmp returns -1/0/1 (like <=>); casecmp? returns a boolean.
# (String#casecmp/casecmp? already work; a couple are kept here as a guard.)
def sy(x); x; end   # symbol receiver
def st(x); x; end   # string receiver

# casecmp orders case-insensitively
p sy(:Foo).casecmp(:foo)      # 0
p sy(:abc).casecmp(:abd)      # -1
p sy(:b).casecmp(:A)          # 1  (case-folded: 'b' > 'a')
p sy(:Foo).casecmp(:foobar)   # -1 (prefix is shorter)

# casecmp? is the boolean case-insensitive equality
p sy(:Foo).casecmp?(:foo)     # true
p sy(:Foo).casecmp?(:FOO)     # true
p sy(:Foo).casecmp?(:bar)     # false

# literal-receiver forms
p :HELLO.casecmp(:hello)      # 0
p :HELLO.casecmp?(:world)     # false

# usable directly in a condition
puts(sy(:Yes).casecmp?(:yes) ? "match" : "no")   # match

# String#casecmp / casecmp? remain correct
p st("Foo").casecmp("FOO")    # 0
p st("Foo").casecmp?("foo")   # true
