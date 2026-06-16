# Storing a poly value (one that holds a String/Integer at runtime) into a
# typed-value hash (Hash[String,String] / Hash[String,Integer]) must coerce the
# poly to the hash's value representation -- the setter takes const char* /
# mrb_int, not sp_RbVal. The receiver is a method-return hash (its value type is
# fixed by the body, so an external poly store can't re-widen it to a poly hash).

def fresh_strs
  h = {}
  h["seed"] = "x"
  h            # Hash[String, String]
end

def fresh_ints
  h = {}
  h["seed"] = 0
  h            # Hash[String, Integer]
end

data = { "k" => "v", "n" => 5 }   # heterogeneous -> poly values

# expression position: a poly string value into a Hash[String,String]
puts(fresh_strs["k"] = data["k"])   # "v"
# expression position: a poly int value into a Hash[String,Integer]
puts(fresh_ints["n"] = data["n"])   # 5

puts "done"
