# `to_i(base)` on a POLY receiver (a value whose static type widened to a union --
# e.g. an array element that may be Integer or String, or a `case v` result that
# returns the un-narrowed scrutinee) raised NoMethodError at runtime: the
# poly-receiver method block is gated on argc == 0, so the one-arg radix form was
# never handled. It now parses the string with the given base, like
# String#to_i(base). No-arg to_i (base 10) is unchanged.
vals = ["ff", 42]        # heterogeneous literal -> poly element type
p vals[0].to_i(16)       # 255
p vals[0].to_i           # base 10: "ff" -> 0

bins = ["101", 0]
p bins[0].to_i(2)        # 5

octs = ["17", 0]
p octs[0].to_i(8)        # 15

def widen(v)
  case v
  when Integer then v
  when String then v
  else 0
  end
end
p widen("2a").to_i(16)   # 42 -- the motivating case/when-dispatch shape

# A radix only applies to String#to_i; when the poly value is a non-String at
# runtime (here the Integer 42), CRuby raises ArgumentError rather than parsing
# the value's string form -- so `42.to_i(16)` is NOT a silent 66.
begin
  p vals[1].to_i(16)
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end
