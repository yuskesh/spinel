# Issue #886: Mutating methods on a frozen string literal raise
# FrozenError per MRI. spinel string literals are always frozen.
# Mutable strings (String.new("...")) keep working via the
# mutable_str arm.
begin
  "hello".insert(0, "X")
  puts "BUG: insert no raise"
rescue FrozenError => e
  puts "insert: " + e.message
end
begin
  "hello".prepend("X")
  puts "BUG: prepend no raise"
rescue FrozenError => e
  puts "prepend: " + e.message
end
begin
  "hello" << "Y"
  puts "BUG: << no raise"
rescue FrozenError => e
  puts "<<: " + e.message
end

# Mutable strings still work.
s = String.new("hi")
s << "!"
puts s
