# frozen_string_literal: true
# String#delete_prefix! / delete_suffix! on a mutable_str mutate in
# place and return the receiver. On a frozen string literal they
# raise FrozenError, consistent with chomp! / upcase! et al.
s = String.new("hello")
s.delete_prefix!("hel")
puts s.to_s

s2 = String.new("hello")
s2.delete_suffix!("llo")
puts s2.to_s

# Non-matching prefix/suffix leaves the string unchanged.
s3 = String.new("hello")
s3.delete_prefix!("xyz")
puts s3.to_s

# Frozen string literals raise FrozenError.
begin
  "hello".delete_prefix!("hel")
  puts "BUG: delete_prefix! no raise"
rescue FrozenError => e
  puts "delete_prefix!: " + e.message
end
begin
  "hello".delete_suffix!("llo")
  puts "BUG: delete_suffix! no raise"
rescue FrozenError => e
  puts "delete_suffix!: " + e.message
end
