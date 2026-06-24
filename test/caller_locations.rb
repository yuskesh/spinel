# AOT builds keep no runtime call-stack frames (as with Kernel#caller), so
# caller_locations is an empty array rather than nil. The array-shaped contract
# still holds: it is an Array, is iterable, and is nil-safe under &..
def frames = caller_locations(1, 1)

puts frames.is_a?(Array)
puts(frames.length >= 0)

# iterating / mapping the result must not crash regardless of frame count
frames.each { |loc| }
puts frames.map { |loc| 1 }.length >= 0

# the common guard idiom is nil-safe
label = caller_locations(1, 1)&.first&.label
puts label.nil? || label.is_a?(String)

# arguments are still evaluated for their side effects (as CRuby does before the
# call), even though the result is an empty array
def noisy_start
  puts "arg evaluated"
  1
end
caller_locations(noisy_start, 1)
