# Sorting an array whose elements are not mutually comparable raises
# ArgumentError, matching CRuby (it previously sorted silently). Arrays are
# built with a variable element so they stay heterogeneous PolyArrays.
str = "x"
num = 5
fl  = 1.5

begin
  a = [1, str, 2]
  a.sort
  puts "no raise (sort)"
rescue ArgumentError
  puts "raised (sort)"
end

begin
  b = [3, str, 1]
  b.sort!
  puts "no raise (sort!)"
rescue ArgumentError
  puts "raised (sort!)"
end

# A poly array of mutually comparable values still sorts.
c = [3, num, 1]
r = c.sort
p r

# Integer and Float are mutually comparable; no raise.
d = [3, fl, 2]
e = d.sort
p e
puts "done"

# min / max over incomparable elements also raise (CRuby parity).
begin
  [1, str, 2].min
  puts "no raise (min)"
rescue ArgumentError
  puts "raised (min)"
end

begin
  [1, str, 2].max
  puts "no raise (max)"
rescue ArgumentError
  puts "raised (max)"
end

# comparable min/max still work, including mixed Integer/Float.
g = [3, num, 1, fl]
p g.min
p g.max
