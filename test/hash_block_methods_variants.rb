# Hash block methods (map / each_value / transform_values / select /
# reject) across the typed-hash variants. sym_int variants already
# worked; this fills in the str_int / sym_str / str_str arms that
# previously fell through to the unresolved-call path.
puts({a: 1, b: 2}.map { |k, v| v * 10 }.inspect)
puts({a: 1, b: 2}.transform_values { |v| v * 10 }.inspect)
puts({a: 1, b: 2, c: 3}.select { |k, v| v > 1 }.inspect)
puts({"a" => 1, "b" => 2}.map { |k, v| v * 10 }.inspect)
puts({"a" => 1, "b" => 2}.transform_values { |v| v * 10 }.inspect)
puts({a: "x", b: "y"}.map { |k, v| v }.inspect)
puts({a: "x", b: "y"}.transform_values { |v| v + "!" }.inspect)
puts({"a" => "x", "b" => "y"}.map { |k, v| v }.inspect)
puts({"a" => "x", "b" => "y"}.transform_values { |v| v + "!" }.inspect)
{a: 1, b: 2}.each_value { |v| puts v }
{"a" => 1}.each_value { |v| puts v }
