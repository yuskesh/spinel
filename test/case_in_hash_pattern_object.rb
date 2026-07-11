# Hash-pattern deconstruction in case/in: Struct/Data scrutinees route
# through the synthesized deconstruct_keys, and the pattern-bound locals
# carry usable types (boxed for object members, the variant's value type
# for hash scrutinees).

D = Data.define(:x, :y)
def dsum(d); case d; in {x:, y:}; x + y; end; end
p dsum(D.new(x: 3, y: 4))

# a subset of the keys matches
def dx(d); case d; in {x:}; x; end; end
p dx(D.new(x: 9, y: 1))

# literal value constraint picks the arm; bindings still work
def dlit(d)
  case d
  in {x: 0, y:}; "zero #{y}"
  in {x:, y:}; "x #{x} y #{y}"
  end
end
p dlit(D.new(x: 0, y: 5))
p dlit(D.new(x: 2, y: 5))

S = Struct.new(:a, :b)
def ssum(s); case s; in {a:, b:}; a + b; end; end
p ssum(S.new(10, 20))

# an unmatched constraint falls through to else
def dnm(d)
  case d
  in {x: 100}; "hundred"
  else; "other"
  end
end
p dnm(D.new(x: 1, y: 2))

# hash scrutinee bindings keep the variant's value type
def hsum(h); case h; in {a:, b:}; a + b; end; end
p hsum({a: 1, b: 2})

def hjoin(h); case h; in {n:}; n * 2; end; end
p hjoin({n: "ab"})

# guards can read the bound variables
def dguard(d)
  case d
  in {x:, y:} if x > y; "x>y"
  in {x:, y:}; "x<=y"
  end
end
p dguard(D.new(x: 5, y: 1))
p dguard(D.new(x: 1, y: 5))
