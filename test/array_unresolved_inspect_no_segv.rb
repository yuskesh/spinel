# Issue #1244: an unresolved Array method (here repeated_permutation, which
# Spinel does not implement) used to emit a NULL placeholder whose chained
# .to_a.inspect dereferenced a->len and SEGVed; later it silently printed
# "[]". Under the NoMethodError gate the chain now raises instead -- pin
# that the unresolved path neither crashes nor silently degrades. (The
# message names the first gated call in the chain, not necessarily
# repeated_permutation itself.)
begin
  puts [1, 2].repeated_permutation(2).to_a.inspect
rescue NoMethodError
  puts "raised NoMethodError"
end
puts "after"
