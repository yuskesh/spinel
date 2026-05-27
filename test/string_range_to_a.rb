# String range to_a — single-char ASCII case (the common subset
# shape). Multi-char ranges need full String#succ with cross-position
# carry and are out of scope for now (the runtime helper returns
# an empty array on multi-char input).
puts ("a".."c").to_a.inspect
puts ("A".."E").to_a.inspect
puts ("0".."5").to_a.inspect

# Exclusive range
puts ("a"..."c").to_a.inspect
