# Finite-range Enumerable coverage: select/reject/filter (the fused-loop forms)
# and min_by/max_by, which materialize the range to an int array. Receivers go
# through a method param so the range value reaches the runtime path.
def r(x); x; end

p r(1..5).select { |n| n.even? }
p r(1..5).reject { |n| n.even? }
p r(1..6).filter { |n| n > 3 }
p r(1..5).select { |n| n > 10 }
p r(1...5).select { |n| n.odd? }

p r(1..5).min_by { |n| -n }
p r(1..5).max_by { |n| -n }
p r(2..8).min_by { |n| (n - 5).abs }
p r(1..4).max_by { |n| n % 3 }
