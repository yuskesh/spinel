# A case/in expression whose clauses return different types unifies to a poly
# value, so a binding arm keeps its own type instead of being coerced to a
# sibling arm's type. The scrutinee flows through a helper to defeat folding.
def s(x); x; end

# value-pattern arm returns String, binding arm returns Integer
r1 = case s(9)
     in 0 then "zero"
     in n then n
     end
p r1                       # 9 (bare Integer, not "9")

# binding arm first (String), value-pattern arm second (Integer)
r2 = case s(1)
     in n then "s#{n}"
     in 2 then 200
     end
p r2                       # "s1"

# three arms mixing String / Integer / the bound value
r3 = case s(5)
     in 0 then "zero"
     in 1 then 100
     in n then n
     end
p r3                       # 5

# a guard arm binding, mixed with a String arm
r4 = case s(7)
     in x if x > 10 then "big"
     in x then x
     end
p r4                       # 7

# the matching arm is the String one -> value is the String
r5 = case s(0)
     in 0 then "zero"
     in n then n
     end
p r5                       # "zero"

# a bare local-variable scrutinee: its type is reset during the inference
# fixpoint, so the binding arm must recover it from the prior iteration
v = 9
r6 = case v
     in 0 then "zero"
     in n then n
     end
p r6                       # 9 (bare Integer, not "9")
