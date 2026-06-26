# A block with 2+ params iterating an array whose elements are sub-arrays
# auto-splats each element across the params (CRuby proc auto-splat), for the
# collecting emitters map/collect, select/filter, reject, and sort_by. A single
# param still binds the whole sub-array. Receivers route through a method param
# so the runtime (poly-array) path runs, not a folded literal.
def s(x); x; end

# --- map / collect: each param gets a positional element ---
p s([["a", 1], ["b", 2]]).map { |k, v| "#{k}#{v}" }   # ["a1", "b2"]
p s([[1, 2], [3, 4]]).map { |a, b| a + b }             # [3, 7]
p s([[1, 2], [3, 4]]).collect { |a, b| a * b }         # [2, 12]

# --- single param keeps the whole sub-array (no splat) ---
p s([[1, 2], [3, 4]]).map { |pair| pair.sum }          # [3, 7]

# --- select / reject return the whole matching elements ---
p s([[1, 2], [3, 1]]).select { |a, b| a < b }          # [[1, 2]]
p s([[1, 2], [3, 1]]).reject { |a, b| a < b }          # [[3, 1]]
p s([[1, 2], [3, 1]]).filter { |a, b| a < b }          # [[1, 2]]

# --- sort_by over the second element ---
p s([["bob", 30], ["amy", 20]]).sort_by { |n, a| a }   # [["amy", 20], ["bob", 30]]
p s([[3, 9], [1, 9], [2, 9]]).sort_by { |a, b| a }     # [[1, 9], [2, 9], [3, 9]]

# --- 3 params over triples ---
p s([[1, 2, 3], [4, 5, 6]]).map { |a, b, c| a + b + c } # [6, 15]

# --- more params than elements: extras are nil ---
p s([[1, 2], [3, 4]]).map { |a, b, c| [a, b, c] }      # [[1, 2, nil], [3, 4, nil]]

# --- empty receiver ---
p s([]).map { |a, b| a + b }                           # []

# --- numbered params auto-splat the same way ---
p s([[1, 2], [3, 4]]).map { _1 + _2 }                  # [3, 7]
