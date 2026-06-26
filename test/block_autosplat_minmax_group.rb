# min_by / max_by / minmax_by / group_by auto-splat a poly array's sub-array
# elements across a 2+-param block, like map/sort_by. The winning element (or
# grouped element) is the whole sub-array; a single param keeps it intact.
# Receivers route through a method param so the runtime path runs. A poly helper
# (sm) and an int-array helper (si) are kept separate: mixing the two element
# kinds through one param would widen it to poly and bypass the typed path.
def sm(x); x; end   # poly (nested) arrays
def si(x); x; end   # int arrays

# --- min_by / max_by over the second element ---
p sm([["a", 3], ["b", 1]]).min_by { |k, v| v }         # ["b", 1]
p sm([["a", 3], ["b", 1]]).max_by { |k, v| v }         # ["a", 3]
p sm([[1, 9], [2, 1]]).min_by { |a, b| a + b }         # [2, 1]

# --- minmax_by returns [min, max] elements ---
p sm([[1, 5], [2, 1], [3, 9]]).minmax_by { |a, b| b }  # [[2, 1], [3, 9]]

# --- group_by keys buckets by the block, storing whole elements ---
p sm([[1, 2], [3, 4], [5, 2]]).group_by { |a, b| b }   # {2=>[[1,2],[5,2]], 4=>[[3,4]]}

# --- 3 params ---
p sm([[1, 2, 3], [4, 5, 6]]).max_by { |a, b, c| a + b + c }  # [4, 5, 6]

# --- single param keeps the whole sub-array (no splat) ---
p sm([[1, 9], [2, 1]]).min_by { |pair| pair[1] }       # [2, 1]

# --- single-element receiver (loop binds once) ---
p sm([[7, 2]]).min_by { |a, b| b }                     # [7, 2]

# --- typed int array still works (no splat) ---
p si([5, 1, 9, 3]).max_by { |x| x }                    # 9

# --- numbered params auto-splat too ---
p sm([[1, 2], [3, 4], [5, 0]]).min_by { _2 }           # [5, 0]
