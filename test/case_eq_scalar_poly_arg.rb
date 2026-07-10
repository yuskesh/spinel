# `===` on a scalar-comparable receiver (bool/int/float/string/symbol) is case
# equality == value equality. When the argument unifies to a poly type across
# call sites while the receiver stays a single scalar type, Spinel previously
# rejected the call; it now boxes the receiver and compares by the poly runtime
# rule (numbers cross-compare, other tags by tag).

# In each method the receiver keeps one scalar type; the second argument varies
# across call sites (Integer / Float / true / nil / String / Symbol), so the
# parameter unifies to poly.
def beq(x, y); x === y; end     # x : bool
p beq(true, 1)        # false (true only case-equals true)
p beq(true, true)     # true
p beq(true, nil)      # false
p beq(false, false)   # true

def ieq(x, y); x === y; end     # x : int
p ieq(1, 1)           # true
p ieq(1, 1.0)         # true  (numeric cross-compare)
p ieq(2, "x")         # false
p ieq(3, nil)         # false

def seq(x, y); x === y; end     # x : string
p seq("a", "a")       # true
p seq("a", :a)        # false
p seq("a", 1)         # false

def yeq(x, y); x === y; end     # x : symbol
p yeq(:sym, :sym)     # true
p yeq(:sym, "sym")    # false
p yeq(:sym, 9)        # false
