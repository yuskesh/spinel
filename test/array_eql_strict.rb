# Array#eql? and Array#uniq compare per element with eql? (class-strict:
# 1 is == to 1.0 but not eql?), matching CRuby.
def e(a, b); a.eql?(b); end
def u(a); a.uniq; end     # mixed (poly) arrays only
def ui(a); a.uniq; end    # int arrays only

p e([1, 2], [1, 2.0])       # false
p e([1, 2], [1, 2])         # true
p e([1, 2], [1, 2, 3])      # false
p e(["a", 1.5], ["a", 1.5]) # true
p [1, 2] == [1, 2.0]        # true (== still coerces)
p u([1.0, 1])               # [1.0, 1]
p ui([1, 1, 2])             # [1, 2]
p u([1, 1.0, "x", "x"])     # [1, 1.0, "x"]
a2 = [2.0, 2]
a2.uniq!
p a2                        # [2.0, 2]
