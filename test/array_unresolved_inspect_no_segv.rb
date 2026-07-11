# Issue #1244: an unresolved Array method (originally repeated_permutation,
# then unimplemented) used to emit a NULL placeholder whose chained
# .to_a.inspect dereferenced a->len and SEGVed; later it silently printed
# "[]"; under the NoMethodError gate it raised. repeated_permutation is now
# implemented, so the chain computes the real value -- keep pinning that the
# chained call neither crashes nor silently degrades.
begin
  puts [1, 2].repeated_permutation(2).to_a.inspect
rescue NoMethodError
  puts "raised NoMethodError"
end
puts "after"
