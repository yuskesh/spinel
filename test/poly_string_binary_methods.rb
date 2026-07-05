# String#bytes / #unpack1 on a value that widened to poly (a method with
# multiple return paths) must dispatch to the String impl, not raise
# "undefined method for poly".
def poly_bin
  return @c if @c
  s = [65, 66, 67, 0, 255].pack('C*')
  @c = s
  s
end
d = poly_bin
puts d.bytes.inspect
puts d[0, 3].bytes.inspect
def poly_txt
  return @t if @t
  s = "abc"
  @t = s
  s
end
puts poly_txt.codepoints.inspect
def poly_hdr
  return @h if @h
  s = [1234].pack('V')
  @h = s
  s
end
puts poly_hdr.unpack1('V')
