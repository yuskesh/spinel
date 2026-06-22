# `next <value>` inside a map block contributes that value, rather than
# dropping the element as a bare `continue` would.
p [1, 2, 3].map { |x| next x * 2 }                                  # [2, 4, 6]

# An interior conditional next with a fall-through tail expression.
p [1, 2, 3, 4].map { |x| next 0 if x.even?; x }                     # [1, 0, 3, 0]

# A next carrying a computed local, mixed with a tail.
p [1, 2, 3].map { |x| y = x * x; next y if x == 2; y + 100 }        # [101, 4, 109]

# String elements with a conditional next.
p ["a", "bb", "ccc"].map { |s| next "X" if s.length == 2; s.upcase } # ["A", "X", "CCC"]

# A next whose value type differs from the tail widens the result to a poly
# array, so the carried value is boxed (not coerced to the tail's scalar type).
p [1, 2, 3].map { |x| next "big" if x > 2; x }                      # [1, 2, "big"]
p [1, 2, 3].map { |x| next x.to_f if x == 2; x * 10 }              # [10, 2.0, 30]

# A plain map (no next) and select/reject keep their behavior.
p [1, 2, 3].map { |x| x + 1 }                                       # [2, 3, 4]
p [1, 2, 3, 4].select { |x| x > 2 }                                 # [3, 4]
p [1, 2, 3, 4].reject(&:even?)                                      # [1, 3]
