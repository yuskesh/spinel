# Well-known Encoding constants resolve to the matching boxed encoding value,
# so `str.encoding == Encoding::UTF_8` compiles and compares (it used to emit
# an sp_Class from the uninitialized-constant fallback into a bool context).
p("x".encoding == Encoding::UTF_8)
p("x".encoding == Encoding::US_ASCII)
p(Encoding::UTF_8 == Encoding::UTF_8)
p(Encoding::UTF_8 == Encoding::BINARY)
s = "abc"
if s.encoding == Encoding::UTF_8
  puts "utf8"
else
  puts "other"
end
