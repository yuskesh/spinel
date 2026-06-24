# `next <cond>` inside a select/reject block decides the element's inclusion,
# instead of being dropped as a bare skip.
p [1, 2, 3, 4].select { |x| next true if x == 2; x > 2 }          # [2, 3, 4]
p [1, 2, 3, 4].reject { |x| next true if x == 1; x.even? }        # [3]
p [1, 2, 3].select { |x| next false if x == 2; true }             # [1, 3]
p ["a", "bb", "ccc"].select { |s| next false if s == "bb"; s.length >= 1 } # ["a", "ccc"]

# An int- or float-valued block is truthy unless it is nil -- 0 / 0.0 select.
p [1, 2, 3, 4].select { |x| x % 2 }                               # [1, 2, 3, 4]
p [1, 2, 3, 4].reject { |x| x % 2 }                               # []
p [1, 2, 3].select { |x| next 0 if x == 2; x }                    # [1, 2, 3]
p [1, 2, 3, 4].select { |x| (x % 2).to_f }                        # [1, 2, 3, 4]

# Plain select/reject (no next) keep their behavior.
p [1, 2, 3, 4].select { |x| x > 2 }                               # [3, 4]
p [1, 2, 3, 4].reject(&:even?)                                     # [1, 3]
