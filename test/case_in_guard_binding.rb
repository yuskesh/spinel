# A destructured array-pattern guard sees the bindings the pattern introduces.
def s(x); x; end
r = case s([1, 2])
    in [a, b] if a + b == 3 then "sum3"
    else "no"
    end
p r
r2 = case s([2, 2])
     in [a, b] if a + b == 3 then "sum3"
     else "no"
     end
p r2
