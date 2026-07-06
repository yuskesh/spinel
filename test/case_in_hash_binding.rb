# case/in in value position with hash-pattern `=> name` bindings must bind.
def s(x); x; end
r = case s({name: "x", age: 5})
    in {name: String => n, age: Integer => a} then [n, a]
    end
p r
r2 = case s({name: "y", age: 9})
     in {name: String => n} then n
     end
p r2
