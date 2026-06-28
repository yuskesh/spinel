# Hash#merge on poly_poly, int_int, and int_str hash variants (str-keyed already
# worked). Frozen-constant receivers infer as poly_poly; ints exercise int-keyed
# variants. A trailing flat_map mirrors the originating gem's use.
A = { "x" => ["a"] }.freeze
B = { "y" => ["b"] }.freeze
C = A.merge(B).flat_map { |k, vs| vs.map { |v| k + v } }
puts C.length
puts C.sort.join(",")

ii = { 1 => 10 }.merge({ 2 => 20 })
puts ii.length
puts ii[2]

is = { 1 => "one" }.merge({ 2 => "two" })
puts is[1]
puts is[2]

# later keys win on overlap (receiver value overwritten by argument)
ov = { 1 => 10 }.merge({ 1 => 99, 3 => 30 })
puts ov[1]
puts ov.length
