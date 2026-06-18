# h[k] ||= v / &&= / op= on a hash parameter passed in empty ({} infers as an
# unresolved element type): the index-or-write usage now seeds the parameter's
# hash variant from the key and value types, so the method's return type
# resolves instead of degrading to poly.
def fill(h, k)
  h[k] ||= "default"
  h[k]
end
puts fill({}, "name")
puts fill({ "name" => "set" }, "name")   # existing key: ||= keeps it

def tally(h, k)
  h[k] ||= 0
  h[k] += 1
  h[k]
end
puts tally({}, "x")

def memo(h, k)
  h[k] &&= "present"
  h[k].inspect
end
puts memo({}, "y")                       # &&= on a missing key stays nil

def symfill(h, k)
  h[k] ||= "sym"
  h[k]
end
puts symfill({}, :a)                     # symbol-keyed hash
