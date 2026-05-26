# Issue #722: a string literal with an embedded NUL byte reports
# the full length, and .bytes iterates all bytes (not strlen-
# truncated at the NUL). Pre-fix the parser AST text format also
# truncated the field at the NUL.
puts "hello\0world".length
puts "hello\0world".bytes.length
puts "a\0b\0c".length
puts "\0\0\0".length
