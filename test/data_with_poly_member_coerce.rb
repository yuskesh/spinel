# A poly (sp_RbVal) value assigned to a CONCRETE record member via Data#with was
# passed straight into the generated constructor's typed slot (const char* /
# mrb_int / mrb_float / ...) with no coercion -- a C type error at compile time --
# even though the regular `.new` call path (emit_arg_or_default) already coerces
# such an argument. Data#with now applies the same poly->concrete coercion.
M = Data.define(:entry, :xs)
m = M.new(entry: "", xs: [])
msg = [:entry, "hi"]              # a heterogeneous [Symbol, String] array
p m.with(entry: msg[1]).entry     # msg[1] is the array's union (poly) element

N = Data.define(:n, :f, :x)
nn = N.new(n: 0, f: 0.0, x: 0)
ints = [10, 20]
flts = [1.5, 2.5]
p nn.with(n: ints[0]).n           # poly -> Int member
p nn.with(f: flts[1]).f           # poly -> Float member

B = Data.define(:flag)
bb = B.new(flag: false)
mix = [true, 1]                   # heterogeneous [Bool, Int] -> poly element
p bb.with(flag: mix[0]).flag      # poly -> Bool member (coerced via sp_poly_truthy)
