# Issue #723. `.encoding` returns the source label (spinel uses
# UTF-8 throughout, so the constant "UTF-8" is the right answer).
# spinel doesn't model Encoding objects -- the return is a plain
# string, not an Encoding instance.

puts "hello".encoding
puts "x".encode.encoding
puts "y".b.encoding
