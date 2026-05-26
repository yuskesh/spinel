h_sym_int = { a: 1, b: 2, c: 3 }
h_sym_int.each_key { |k| puts k }
puts "-"
h_sym_int.each_value { |v| puts v }
puts "-"

h_str_int = { "a" => 1, "b" => 2 }
h_str_int.each_key { |k| puts k }
puts "-"
h_str_int.each_value { |v| puts v }
puts "-"

h_sym_str = { x: "one", y: "two" }
h_sym_str.each_key { |k| puts k }
puts "-"
h_sym_str.each_value { |v| puts v }
