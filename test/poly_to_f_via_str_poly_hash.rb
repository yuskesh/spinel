# sp_poly_to_f lacked the SP_TAG_STR arm its sibling sp_poly_to_i has
# (strtoll), so `.to_f` on a poly-typed string value returned 0.0
# instead of parsing the number. The typed String#to_f path (atof) was
# unaffected, which makes the miss silent: the same source parses fine
# under CRuby and with a statically-string receiver, then yields 0.0
# only when the receiver is poly.
#
# Repro shape from a JSON-ish Hash[String, untyped]: read a string
# value out of a heterogeneous hash and call .to_f on it.

class C
  def initialize
    @h = { "w" => "900", "h" => "600.5", "pad" => " 2.5", "junk" => "abc", "blank" => "", "n" => 7, "f" => 1.25 }
  end

  def at(k)
    (@h[k]).to_f
  end
end

c = C.new
puts c.at("w")
puts c.at("h")
puts c.at("pad")
puts c.at("junk")
puts c.at("blank")
puts c.at("n")
puts c.at("f")
