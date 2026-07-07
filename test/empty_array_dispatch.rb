# Iterators dispatched on an empty array literal receiver. Previously the empty
# `[]` had an unresolved type, so the method aborted at runtime ("undefined
# method 'each' for unknown") and the exception unwound past following code.

# The canonical case: `each` runs zero times and control continues.
[].each { |x| print x }
print "done\n"

# Collection iterators over an empty literal produce the empty result without
# ever running the block.
p([].map { |x| x + 1 })
p([].collect { |x| x * 2 })
p([].select { |x| x > 0 })
p([].filter { |x| x.even? })
p([].reject { |x| x > 0 })
p([].filter_map { |x| x if x > 0 })
p([].flat_map { |x| [x, x] })
p([].find { |x| x > 0 })
p([].each_with_object([]) { |x, acc| acc << x })
p([].group_by { |x| x })
p([].sort_by { |x| -x })
p([].min_by { |x| x })
p([].max_by { |x| x })

# Non-block / folded forms already worked and must keep working.
p [].sum
p [].min
p [].first

# The empty literal used as an assigned value still back-fills its element type.
a = []
a << 1
a << 2
p a
p a.sum
