# A case/in array pattern must check required element values, not only length.
def si(x); x; end
def ss(x); x; end

r1 = case si([1, 2, 3])
     in [9, *] then "matched"
     else "no"
     end
p r1                       # "no" -- head 1 != 9

r2 = case si([1, 2, 3])
     in [1, *] then "matched"
     else "no"
     end
p r2                       # "matched" -- head 1 == 1

r3 = case si([1, 2, 3])
     in [1, 2, 3] then "exact"
     in [1, 2, x] then "prefix"
     else "no"
     end
p r3                       # "exact"

r4 = case si([1, 2, 9])
     in [1, 2, 3] then "exact"
     in [1, 2, x] then "bound #{x}"
     else "no"
     end
p r4                       # "bound 9" -- falls to the binding clause, binds x

r5 = case si([1, 2, 3])
     in [1, b, 3] then "mid #{b}"
     else "no"
     end
p r5                       # "mid 2" -- literal ends match, middle binds

r6 = case si([1, 2, 3])
     in [1, b, 9] then "mid #{b}"
     else "no"
     end
p r6                       # "no" -- trailing literal 9 != 3

r7 = case ss(["x", "y"])
     in ["x", *] then "x-head"
     else "no"
     end
p r7                       # "x-head"

r8 = case ss(["x", "y"])
     in ["z", *] then "z-head"
     else "no"
     end
p r8                       # "no" -- "x" != "z"
