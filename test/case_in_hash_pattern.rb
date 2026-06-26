# A hash pattern in `case/in` binds each value target (shorthand `{k:}`, `{k: v}`,
# and capture `{k: Class => v}`) and matches only when the scrutinee is a hash
# holding every key with each value matching its sub-pattern. Previously the
# bindings came back nil. Each scrutinee routes through its own method param so
# the runtime path runs; separate helpers keep each hash variant concrete.

# class check + capture binding (the canonical form)
def h1(x); x; end
case h1({name: "x", age: 5})
in {name: String => n, age: Integer => a} then p [n, a]   # ["x", 5]
end

# shorthand binding `{k:}`
def h2(x); x; end
case h2({a: 1, b: 2})
in {a:, b:} then p [a, b]                                  # [1, 2]
end

# value class selects the arm
def h3(x); x; end
case h3({k: 5})
in {k: String} then p :str
in {k: Integer} then p :int                               # :int
end

# missing key -> no match
def h4(x); x; end
case h4({a: 1})
in {b:} then p :y
else p :no                                                 # :no
end

# value-binding to a named local (not shorthand)
def h5(x); x; end
case h5({n: "bob", role: "dev"})
in {n: name, role: r} then p [name, r]                    # ["bob", "dev"]
end

# a non-matching class on a present key -> fall through
def h6(x); x; end
case h6({id: "abc"})
in {id: Integer => i} then p i
else p :nomatch                                            # :nomatch
end

# value bound from a poly-valued hash where the local is used as an array: the
# binding must coerce to the array type, not assign a boxed value to a pointer.
def h7(x); x; end
case h7({items: [1, 2, 3], tag: "t"})
in {items: arr} then p arr.sum                            # 6
end
