# String#ord on empty string should raise ArgumentError, not return 0.
begin
  "".ord
  puts "BUG: no raise"
rescue ArgumentError => e
  puts "ArgumentError: " + e.message
end

# Normal ord still works
p "A".ord
p "a".ord
