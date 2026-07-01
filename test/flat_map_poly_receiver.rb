# flat_map on a poly receiver -- a value whose array-ness is only known at
# runtime (a recursive parameter, or a `case ... end` whose arms mix arrays and
# scalars). The receiver is coerced to a poly array at runtime, so the recursive
# tree-flattening idiom works.
def flatten_tree(a)
  a.flat_map { |node| node.is_a?(Array) ? flatten_tree(node) : node }
end
p flatten_tree([1, [2, [3, 4]], 5])        # [1, 2, 3, 4, 5]

def prep(dish)
  case dish
  in [*head, :a, :b, *tail]
    [head, :ab, tail]
  else
    dish
  end.flat_map { |node| node.is_a?(Array) ? prep(node) : node }
end
p prep([:x, :a, :b, :y])                    # [:x, :ab, :y]
p prep([:p, :q])                            # [:p, :q]
