# An empty `{}` literal defaults to a StrPolyHash, but when a hash-returning
# method (or a hash-typed ternary) has another branch of a different hash
# variant, the empty branch must take that variant -- otherwise the StrPolyHash*
# is an incompatible pointer type against e.g. a SymPolyHash return.
def pick_sym(c)
  c ? {} : { a: 1 }            # SymPolyHash; empty branch must match
end
def pick_str(c)
  c ? {} : { "k" => "v" }      # str-keyed; empty branch must match
end
def via_local(c)
  h = c ? {} : { a: 1 }        # ternary in assignment position
  h.empty?
end

puts pick_sym(true).empty?     # true
puts pick_sym(false)[:a].to_s  # 1
puts pick_str(true).empty?     # true
puts pick_str(false)["k"]      # v
puts via_local(true)           # true
puts via_local(false)          # false
