# Kernel#Float / #Integer strictness, CRuby-parity edges:
# - digit-separating underscores parse ("1_000.5"); misplaced ones raise
# - an embedded NUL is invalid (a C-string scan would parse the prefix)
# - ".5" and "5." both parse (CRuby 4.0 accepts a trailing dot)
# - hex Float literals parse, including fractional and p-exponent forms
#   ("0x1_1" -> 17.0, "0x1_1.0" -> 17.0, CRuby 4.0)
# - "inf"/"nan" raise (strtod would parse them)
# - Integer() auto-detects prefix bases, including leading-0 octal ("077" -> 63)
def tf(s)
  Float(s)
rescue ArgumentError
  "AE"
end
def ti(s)
  Integer(s)
rescue ArgumentError
  "AE"
end
["1_000.5", "1_0.5_5", "1e1_0", "0x1_1", "0x1_1.0", "1__0", "1_", "_1",
 "1_e3", "1e_3", ".5", "5.", "inf", "Infinity", "nan", " +3.25 "].each do |s|
  puts "F #{s.inspect} #{tf(s).inspect}"
end
puts "F NUL #{tf("1\0").inspect}"
["1_000", "077", "0x1A", "0b101", " -7 ", "1__0"].each do |s|
  puts "I #{s.inspect} #{ti(s).inspect}"
end
puts "I NUL #{ti("1\0").inspect}"
