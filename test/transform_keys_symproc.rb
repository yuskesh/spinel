# Hash#transform_keys whose block changes the key to a Symbol must produce a
# symbol-keyed hash, not keep the receiver's string-keyed variant.
#
# `{ "a" => 1 }.transform_keys(&:to_sym)` inferred the result as the receiver's
# type (there is no sym-scalar hash variant, so ty_hash_of(Symbol, Int) was
# UNKNOWN and fell back to the string-keyed receiver type), then emitted a
# string-keyed hash and passed the transformed sp_sym key to a const char *
# setter -- an incompatible-pointer C error. Symbol keys yield a SymPolyHash.

# 1. String keys -> symbols (int and string values).
p({ "a" => 1, "b" => 2 }.transform_keys(&:to_sym))      #=> {a: 1, b: 2}
p({ "a" => "x", "b" => "y" }.transform_keys(&:to_sym))  #=> {a: "x", b: "y"}

# 2. Symbol keys -> strings.
p({ a: 1, b: 2 }.transform_keys(&:to_s))                #=> {"a" => 1, "b" => 2}

# 3. Integer keys -> strings.
p({ 1 => "a", 2 => "b" }.transform_keys(&:to_s))        #=> {"1" => "a", "2" => "b"}

# 4. An explicit block still works.
p({ "a" => 1 }.transform_keys { |k| k.upcase })         #=> {"A" => 1}

# 5. Through a method param.
def tk(h) = h.transform_keys(&:to_sym)
p tk({ "x" => 10, "y" => 20 })                          #=> {x: 10, y: 20}

puts "done"
