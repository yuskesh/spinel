# Compound assignment to a nested (2-D) indexed slot: `m[a][c] OP= v`.
# The receiver `m[a]` is a poly-typed index expression, so the op-write
# goes through the poly-receiver arm of IndexOperatorWriteNode with an
# int (or dynamic) key. Previously only sym/str keys were supported and
# this failed to compile with "unsupported index operator assignment
# (poly-recv, non-sym/str key)".

# Float elements, int keys via loop variables.
def accumulate
  m = [[0.0, 0.0], [0.0, 0.0]]
  a = 0
  while a < 2
    c = 0
    while c < 2
      m[a][c] += 1.5
      c += 1
    end
    a += 1
  end
  m
end
p accumulate            # [[1.5, 1.5], [1.5, 1.5]]

# Int elements, several operators; receiver index is a computed expression.
def int_ops
  m = [[8, 9], [10, 11]]
  i = 0
  m[i][0] /= 2
  m[i][1] %= 4
  m[i + 1][0] <<= 2
  m[i + 1][1] |= 4
  m
end
p int_ops               # [[4, 1], [40, 15]]

# Chained ops on the same slot inside a loop.
def chained
  m = [[1, 2], [3, 4]]
  i = 0
  while i < 2
    j = 0
    while j < 2
      m[i][j] *= 10
      m[i][j] -= 1
      j += 1
    end
    i += 1
  end
  m
end
p chained               # [[9, 19], [29, 39]]

# Hash-of-arrays: the inner receiver `h[k]` is poly, the final key is int.
def hash_of_arrays
  h = { "a" => [1, 2] }
  k = "a"
  i = 1
  h[k][i] += 5
  h["a"]
end
p hash_of_arrays        # [1, 7]

# Three levels deep: `m[a][0][1] += v`.
def deep
  m = [[[0, 0]], [[0, 0]]]
  a = 0
  while a < 2
    m[a][0][1] += 7
    a += 1
  end
  m
end
p deep                  # [[[0, 7]], [[0, 7]]]
