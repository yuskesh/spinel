# String#split(" ") is whitespace-split mode (CRuby special case)
# Strips leading whitespace, collapses consecutive whitespace.

# split(" ") is whitespace mode
puts "  hello  world  ".split(" ").inspect
puts "a  b".split(" ").inspect
# no-arg split is also whitespace mode
puts "  hello  world  ".split.inspect
# split(",") is literal mode (preserves empty fields)
puts "a,,b".split(",").inspect
