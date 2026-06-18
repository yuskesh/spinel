# map/each/select.with_index with an optional offset. The receiver is a
# blockless enumerator; with_index binds the element plus a running index.
# Route receivers through per-type method params to exercise the runtime path
# (distinct helpers so the params stay monomorphic, not unified to poly).
def ints(a) = a
def strs(a) = a

nums = ints([10, 20, 30])
p nums.map.with_index { |x, i| x + i }        # [10, 21, 32]
p nums.map.with_index(1) { |x, i| x * i }     # [10, 40, 90]
p nums.collect.with_index(100) { |x, i| i }   # [100, 101, 102]

words = strs(["a", "b", "c", "d"])
p words.map.with_index { |s, i| "#{i}:#{s}" } # ["0:a", "1:b", "2:c", "3:d"]
p words.select.with_index { |s, i| i.even? }  # ["a", "c"]
p words.reject.with_index { |s, i| i.even? }  # ["b", "d"]

# each.with_index returns the receiver; collect side effects via an accumulator
acc = []
ret = nums.each.with_index(10) { |x, i| acc << (i * 1000 + x) }
p acc                                          # [10010, 11020, 12030]
p ret                                          # [10, 20, 30]

# a runtime-empty but concretely-typed receiver still yields []
p nums.select.with_index { |x, i| i > 99 }     # []
