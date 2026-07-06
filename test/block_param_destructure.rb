# Parenthesized block parameters destructure each array element into the nested
# names: `|a, (b, c), d|`, `|(a, b)|`, nested `(b, (c, d))`, a named splat rest
# `(b, *r)`, and an anonymous splat `(*)`. Works across every block-taking form.

# each: paren param destructures the element
[[1, [2, 3], 4]].each { |a, (b, c), d| p [a, b, c, d] }        # [1, 2, 3, 4]

# a single fully-parenthesized param over each element
[[1, 2], [3, 4]].each { |(a, b)| p [a, b] }                    # [1, 2] then [3, 4]

# leading paren then a trailing plain param
[[[1, 2], 3]].each { |(a, b), c| p [a, b, c] }                 # [1, 2, 3]

# nested destructuring
[[1, [2, [3, 4]]]].each { |a, (b, (c, d))| p [a, b, c, d] }    # [1, 2, 3, 4]

# a named splat rest inside the destructure collects the tail
[[1, [2, 3, 4]]].each { |a, (b, *r)| p [a, b, r] }             # [1, 2, [3, 4]]

# an anonymous splat consumes-and-discards its slot
[[1, 2, 3]].each { |a, (*), b| p [a, b] }                      # [1, 3]

# map returns the block value with destructured params
p([[1, 2], [3, 4]].map { |(a, b)| a + b })                     # [3, 7]

# each_with_index with a destructured first param
[[10, 20], [30, 40]].each_with_index { |(a, b), i| p [a, b, i] }

# each_slice yields sub-arrays; a single |(lo, hi)| splits each slice
p([1, 2, 3, 4].each_slice(2).map { |(lo, hi)| hi - lo })       # [1, 1]
