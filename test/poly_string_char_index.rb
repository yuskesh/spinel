# String#[int] on a value that widened to poly (a method with multiple return
# paths) must return the single character, not nil.
def poly_str
  return @c if @c
  s = "ABCDEF"
  @c = s
  s
end
d = poly_str
puts d[0]
puts d[3]
puts d[-1]
puts d[99].inspect
puts d[0].ord
