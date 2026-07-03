# A method whose value is an element-less hash -- a bare `{}` or `Hash.new` /
# `Hash.new(default)` -- witnesses no element types in its body, so its return
# inferred TY_UNKNOWN and codegen emitted a `void` C function; the caller's
# `x = mk` then failed to compile (#1680). The caller's own use pins the concrete
# hash variant, so back-propagate that onto the method's return.
def mk_str;  Hash.new(""); end
def mk_int;  Hash.new(0);  end
def mk_bare; {};           end
def mk_ret;  return Hash.new(""); end

def mk_top; ::Hash.new(""); end          # top-level ::Hash (ConstantPathNode receiver)

module Tep
  def self.str_hash; Hash.new(""); end   # the roundhouse shape: a Const.cmethod
end

a = mk_str
a["k"] = "v"
puts a["k"]
puts a["miss"].length    # default "" preserved across the method boundary => 0

b = mk_int
b["n"] = 1
puts b["n"]
puts b["miss"]           # default 0 preserved => 0

c = mk_bare
c["x"] = "y"
puts c["x"]

d = mk_ret
d["p"] = "q"
puts d["p"]

e = Tep.str_hash
e["a"] = "b"
puts e["a"]

f = mk_top
f["m"] = "n"
puts f["m"]
