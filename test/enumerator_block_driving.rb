# A materialized Enumerator (from a blockless each / reverse_each / each_char)
# drives its block-taking Enumerable methods off its element array:
# enum.map { } behaves as enum.to_a.map { }. Index-producing enumerators
# (each_with_index / with_index) keep their own dedicated chain handling.

nums = [1, 2, 3, 4, 5, 6]

# --- value-producing Enumerable methods on an Array#each enumerator ---
p nums.each.map { |x| x * 2 }
p nums.each.select { |x| x.even? }
p nums.each.reject { |x| x.even? }
p nums.each.find { |x| x > 3 }
p nums.each.flat_map { |x| [x, -x] }
p nums.each.count { |x| x.even? }
p nums.each.sort_by { |x| -x }
p nums.each.group_by { |x| x % 3 }
p nums.each.partition { |x| x < 3 }
p nums.each.min_by { |x| (x - 3).abs }
p nums.each.max_by { |x| (x - 3).abs }
p nums.each.each_with_object([]) { |x, acc| acc << x * x }
p nums.each.filter_map { |x| x * 10 if x.even? }
p nums.each.all? { |x| x.positive? }
p nums.each.any? { |x| x > 5 }
p nums.each.none? { |x| x > 10 }
p nums.each.take_while { |x| x < 4 }
p nums.each.drop_while { |x| x < 4 }

# --- String#each_char enumerator ---
p "hello".each_char.map { |ch| ch.upcase }
p "hello".each_char.select { |ch| "aeiou".include?(ch) }
p "mississippi".each_char.group_by { |ch| ch }.transform_values(&:size)

# --- reverse_each enumerator ---
p [1, 2, 3].reverse_each.map { |x| x * 10 }

# --- index-producing enumerators keep their dedicated chain handling ---
p [10, 20, 30].each_with_index.map { |x, i| [i, x] }
p [10, 20].each_with_index.to_a
p %w[a b c].each.with_index(1).map { |ch, i| "#{i}:#{ch}" }

# --- a blockless terminal still materializes directly (no delegation needed) ---
p [1, 2, 3].each.to_a
p [1, 2, 3].each.first(2)
