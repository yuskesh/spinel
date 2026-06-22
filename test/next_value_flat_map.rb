# `next [..]` inside a flat_map block flattens its array, exactly like the tail.
p [1, 2, 3].flat_map { |x| next [0] if x == 2; [x, x] }        # [1, 1, 0, 3, 3]
p [1, 2, 3].flat_map { |x| [x, x * 10] }                       # [1, 10, 2, 20, 3, 30]
p ["a", "b"].flat_map { |s| next ["X"] if s == "b"; [s, s] }   # ["a", "a", "X"]
