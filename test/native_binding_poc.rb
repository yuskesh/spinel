# Path B PoC: a user module declares a typed native binding to carried C
# (here lib/sp_json.c's sp_json_val, already in libspinel_rt.a). No hardcoded
# compiler knowledge of this module — the DSL declaration alone drives return
# type inference (:string) and emission (Mod.method(x) -> sp_json_val(<boxed x>)).
module NB
  native_func :gen, [:any], :string, "sp_json_val"
end

# return type is :string, so this compiles in a String context and concatenates
puts NB.gen({"a" => 1, "b" => [2, 3]}) + "!"
puts NB.gen(42)
puts NB.gen(["x", nil, true])
s = NB.gen({"k" => "v"})
puts s.length
