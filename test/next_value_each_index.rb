# `next <value>` through each.with_index collectors: map contributes the value,
# select/reject decide inclusion, the scalar terminals read the value.
p [10, 20, 30].each_with_index.map { |x, i| next 0 if i == 1; x + i }       # [10, 0, 32]
p [10, 20, 30].each_with_index.map { |x, i| x + i }                         # [10, 21, 32]
p [10, 20, 30].each_with_index.select { |x, i| next true if i == 0; x > 15 } # [[10,0],[20,1],[30,2]]
p [1, 2, 3, 4].each_with_index.count { |x, i| next true if i == 0; x.even? } # 3
p [2, 4, 6].each_with_index.all? { |x, i| next true if i == 2; x.even? }     # true
p [10, 20, 30].each_with_index.map { |x, i| }                               # [nil, nil, nil] (empty body)
