# Hashes compare by VALUE across storage variants, like arrays: Ruby has one
# Hash, and a runtime-built poly hash (JSON.parse) equals the same pairs
# written as a typed literal. Previously poly==typed-hash and mixed-variant
# hash==hash both constant-folded to false, and .class on a boxed hash
# printed an empty name.
require "json"
a = JSON.parse('{"k": 1}')
b = { "k" => 1 }
p a == b
p b == a
p a != b
puts a.class
p JSON.parse('{"k": "s", "n": [1, 2]}') == { "k" => "s", "n" => [1, 2] }
p JSON.parse('{"k": 1}') == { "k" => 2 }
p JSON.parse('{"k": 1}') == { "j" => 1 }
p JSON.parse('{"k": 1}') == { "k" => 1, "j" => 2 }
p JSON.parse('{"k": 1}') == [1]
p JSON.parse('{"k": 1}') == { "k" => 1.0 }   # 1 == 1.0
