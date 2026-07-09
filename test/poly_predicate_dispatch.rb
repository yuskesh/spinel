# all?/any?/none?/one? on a POLY receiver (a value the analyzer can only type
# as poly, here a heterogeneous hash get) dispatch over the boxed array at
# runtime instead of raising "undefined method for poly".
h = { "nums" => [1, 2, 3], "label" => "x" }
vals = h["nums"]
p vals.all? { |v| !v.nil? }
p vals.any? { |v| v.nil? }
p vals.none? { |v| v.nil? }
p vals.one? { |v| !v.nil? }
