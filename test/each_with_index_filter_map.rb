# Enumerator#filter_map over each_with_index: keep truthy block values.
arr = [10, 20, 30, 40]
puts arr.each_with_index.filter_map { |s, i| i if s > 15 }.inspect
puts arr.each_with_index.filter_map { |s, i| s + i if i.even? }.inspect
puts arr.each_with_index.filter_map { |s, i| "#{i}:#{s}" if s < 35 }.inspect

# A numeric 0 / 0.0 (and "") block value is truthy in Ruby: filter_map keeps
# it, dropping only nil/false. A C zero-falsy test would wrongly discard them.
nums = [3, 0, 5]
puts nums.each_with_index.filter_map { |v, i| v }.inspect
puts nums.each_with_index.filter_map { |v, i| v * i * 1.0 }.inspect
puts nums.each_with_index.filter_map { |v, i| v.to_s }.inspect

# select/reject/count/any?/all? share the same truthiness path: a block that
# yields a bare 0 is truthy, so select keeps every pair and reject none.
puts [7, 0, 9].each_with_index.select { |v, i| v - v }.inspect
puts [7, 0, 9].each_with_index.reject { |v, i| v - v }.inspect
puts [7, 0, 9].each_with_index.count { |v, i| v - v }
puts [7, 0, 9].each_with_index.all? { |v, i| v - v }
puts [7, 0, 9].each_with_index.any? { |v, i| v - v }
