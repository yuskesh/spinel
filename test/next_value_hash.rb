# `next <value>` through hash map/select/reject: map contributes the value;
# select/reject decide inclusion by the (next-aware) block value.
p({ a: 1, b: 2, c: 3 }.map { |k, v| next 0 if v == 2; v * 10 })         # [10, 0, 30]
p({ a: 1, b: 2 }.map { |k, v| "#{k}=#{v}" })                            # ["a=1", "b=2"]
p({ a: 1, b: 2, c: 3 }.select { |k, v| next true if k == :b; v > 2 })   # {b: 2, c: 3}
p({ a: 1, b: 2, c: 3 }.reject { |k, v| next true if k == :a; v.even? }) # {c: 3}
p({ "x" => 1, "y" => 2 }.map { |k, v| [k, v * 2] })                     # [["x", 2], ["y", 4]]
