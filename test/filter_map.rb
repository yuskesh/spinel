# Enumerable#filter_map (Ruby 2.7+): map, then drop falsy results, in one pass.
# The result is a (boxed) array of the kept values; output matches whether the
# block body is a nilable `expr if cond` or a plain transform.
p [1, 2, 3, 4].filter_map { |x| x * 2 if x.even? }   # [4, 8]
p [1, 2, 3].filter_map { |x| x * 2 }                 # always truthy -> like map
p ["a", "", "b", ""].filter_map { |s| s.upcase unless s.empty? }
p [1, 2, 3, 4, 5].filter_map { |x| x if x > 2 }

# assigned and chained
evens_doubled = [1, 2, 3, 4, 5, 6].filter_map { |x| x * 2 if x.even? }
p evens_doubled
p [1, 2, 3, 4, 5, 6].filter_map { |x| x * 2 if x.even? }.sum

# nil and false are both dropped
p [1, 2, 3].filter_map { |x| x.even? ? x : false }
