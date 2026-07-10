# String#[]= beyond the single integer index: Range, (start, length),
# substring, and Regexp forms, plus the raising misses.
s = "hello"
s[1..2] = "XY"
p s
s = "hello"
s[1, 2] = "XY"
p s
s = "hello"
s["ell"] = "ELL"
p s
s = "hello"
s[/l+/] = "L"
p s
s = "hello"
s[0] = "H"
p s
s = "abcdef"
s[-2, 2] = "!"
p s
s = "abc"
s[3..5] = "def"
p s
begin
  s = "abc"
  s["zz"] = "x"
rescue IndexError => e
  p e.message
end
begin
  s = "abc"
  s[/z+/] = "x"
rescue IndexError => e
  p e.message
end
