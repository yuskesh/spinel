# String#% with a right-hand side whose static type is poly (a scalar at one
# call site, an Array at another) must spread an Array value across the format
# directives at runtime. Spinel previously wrapped a poly RHS in a one-element
# array unconditionally, so an Array read zeros.
def fmt(f, v); f % v; end

# v unifies to poly: it receives an Integer, an Array, a Float, and a String.
p fmt("%d", 5)               # "5"       (scalar stays a one-element list)
p fmt("%d-%d", [1, 2])       # "1-2"     (Array spread across directives)
p fmt("%03d/%02d", [7, 3])   # "007/03"
p fmt("%.1f", 2.5)           # "2.5"
p fmt("%s!", "hi")           # "hi!"
p fmt("%s=%s", ["k", "v"])   # "k=v"     (string array spread)
p fmt("%d %s", [1, "x"])     # "1 x"     (mixed-type array spread)

# A single-element array still fills one directive.
p fmt("[%d]", [9])           # "[9]"
