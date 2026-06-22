# `next <value>` through each_slice(n).map contributes the value.
p [1, 2, 3, 4].each_slice(2).map { |a, b| next 0 if a == 1; a + b }   # [0, 7]
p [1, 2, 3, 4, 5].each_slice(2).map { |a, b| a }                      # [1, 3, 5]
