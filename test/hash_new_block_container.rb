# Hash.new with a block whose body assigns a container default used to
# miscompile: the empty `[]`/`{}` literal infers as an untyped value, so the
# emitted default-proc declared `void _t = sp_IntArray_new()`. The value is
# now boxed into an sp_RbVal temp, so container defaults compile and work.
# (String/Integer keys are used so #inspect matches CRuby; a symbol-keyed
#  Hash.new{} default still renders keys with `=>` -- tracked separately.)

# array default
words = Hash.new { |h, k| h[k] = [] }
words["a"] << 1
words["a"] << 2
words["b"] << 3
p words

# integer default
counts = Hash.new { |h, k| h[k] = 0 }
counts["x"] += 1
counts["x"] += 1
counts["y"] += 5
p counts

# hash default (nested)
nested = Hash.new { |h, k| h[k] = {} }
nested["a"]["b"] = 1
nested["a"]["c"] = 2
p nested

# the block value is returned (auto-vivification returns the new container)
fresh = Hash.new { |h, k| h[k] = [] }
fresh["z"].push(9)
p fresh["z"]
