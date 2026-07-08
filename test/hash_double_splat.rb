# A hash literal with a double-splat (`{ **h, k: v }`) must merge the spread
# source instead of erasing the literal's type to UNKNOWN and rejecting.
#
# Type inference bailed the moment it saw a non-AssocNode element, so `**h`
# gave the literal no inferrable key/value type. Merge the spread source's
# key/value types (a concrete hash) -- or, for a source reached through a poly
# binding, iterate its boxed pairs at runtime into a poly hash.

# 1. The gap: a poly spread source (h flows through a poly array via map) plus
#    an explicit key computed from it.
def bump(h)
  { **h, x: h[:x] + 1 }
end
rows = (0...3).map { |i| { x: i.to_f, y: 0.0 } }
p rows.map { |h| bump(h) }         #=> [{x: 1.0, y: 0.0}, {x: 2.0, y: 0.0}, {x: 3.0, y: 0.0}]

# 2. A concretely-typed spread source with an overriding key.
def upd(h) = { **h, a: 99 }
p upd({ a: 1, b: 2 })              #=> {a: 99, b: 2}

# 3. Splat last: the spread overrides an earlier explicit key.
def upd2(h) = { a: 1, **h }
p upd2({ a: 5, c: 3 })             #=> {a: 5, c: 3}

# 4. Two spreads.
def merge2(a, b) = { **a, **b }
p merge2({ x: 1 }, { y: 2 })       #=> {x: 1, y: 2}

# 5. String-keyed spread + an added string key.
def strh(h) = { **h, "z" => 9 }
p strh({ "a" => 1 })               #=> {"a" => 1, "z" => 9}

# 6. Direct local spread source (concrete type).
h = { x: 1.0, y: 2.0 }
p({ **h, z: 3.0 })                 #=> {x: 1.0, y: 2.0, z: 3.0}

puts "done"
