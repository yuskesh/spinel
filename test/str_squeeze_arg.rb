# String#squeeze with charset argument
# The argument restricts which characters get squeezed.
# "a" squeezes only 'a', "^a" squeezes everything except 'a'.

# no-arg squeeze (all consecutive)
puts "aaabbbccc".squeeze.inspect
# single char
puts "aaabbbccc".squeeze("a").inspect
# multiple chars
puts "aaabbbccc".squeeze("ab").inspect
# range
puts "aaabbbccc".squeeze("a-c").inspect
# negated set
puts "aaabbbccc".squeeze("^a").inspect
# no change case
puts "abc".squeeze("a").inspect
