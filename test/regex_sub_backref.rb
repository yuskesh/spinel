# Issue #855: sub / gsub expand `\1`..`\9` and `\&` backreferences
# in the replacement string.
puts "hello world".sub(/(\w+)/, '\1_up')
puts "hello world".gsub(/(\w+)/, '\1!')
puts "abc".sub(/b/, '[\&]')
# Swap two captured groups via \2\1.
puts "foo:bar".sub(/(\w+):(\w+)/, '\2:\1')
# `\\` is a literal backslash, not a backref.
puts "x".sub(/x/, '\\')
# Numeric backref beyond captured groups stays empty.
puts "ab".sub(/(a)(b)/, '\1-\2-\3')
