# String#lines(chomp: true) strips line endings from each line.
# Without the chomp: keyword, line endings are preserved.

puts "a\nb\nc".lines(chomp: true).inspect
puts "a\nb\nc".lines.inspect
puts "a\r\nb\r\nc\r\n".lines(chomp: true).inspect
