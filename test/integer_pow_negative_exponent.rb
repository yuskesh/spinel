# Integer ** with a negative exponent has no Rational result in Spinel, so it
# raises RangeError instead of returning (1/2). Non-negative integer ** and
# Float ** are unchanged. See docs/INCOMPATIBILITIES.md.
puts 2 ** 10
puts 0 ** 0
puts 3 ** 4
puts(2.0 ** -1)
begin
  puts 2 ** -1
rescue RangeError => e
  puts "raised: #{e.message}"
end
begin
  2 ** -3
rescue StandardError => e
  puts "is_standard_error: #{e.is_a?(StandardError)}"
end
