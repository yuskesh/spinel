# A pattern guard (`in PATTERN if GUARD` / `unless GUARD`) can reference the
# bindings introduced by the pattern. Receivers go through per-type identity
# methods to defeat constant folding while keeping each scrutinee monomorphic.
# Binding names are unique per arm so cross-arm type unification can't muddy them.
def ai(x); x; end
def aii(x); x; end
def hh(x); x; end
def ii(x); x; end

# array pattern + if guard referencing the bindings
case ai([1, 2])
in [p1, q1] if p1 + q1 == 3 then p "sum3"
else p "no"
end                                       # "sum3"

case ai([1, 2])
in [p2, q2] if p2 + q2 == 99 then p "yes"
else p "fell-through"
end                                       # "fell-through"

# three-element array, guard chooses among arms
case aii([1, 2, 3])
in [p3, q3, r3] if p3 + q3 + r3 == 6 then p "six"
in [p3b, q3b, r3b] then p "other"
end                                       # "six"

# hash pattern + guard over its typed-capture bindings
case hh({ name: "x", age: 5 })
in { name: String => nm, age: Integer => ag } if ag > 3 then p [nm, ag]
else p "no"
end                                       # ["x", 5]

# unless guard negates
case ai([1, 2])
in [p4, q4] unless p4 == q4 then p "diff"
else p "same"
end                                       # "diff"

# a plain local-binding pattern with a guard still works
case ii(7)
in n5 if n5 > 5 then p "big"
else p "small"
end                                       # "big"

# a guard of a concrete (non-bool) type follows Ruby truthiness: 0 is truthy,
# so an Integer-valued guard that evaluates to 0 still passes (C would treat 0
# as falsy without the truthiness wrapper).
case ii(5)
in n6 if n6 - 5 then p "truthy-zero"
else p "wrongly-falsy"
end                                       # "truthy-zero"
