# A binary String reaching IO#write through a value whose inferred type is
# TY_POLY (String | nil -- the shape a nil-on-miss getter has) was sized with
# strlen, so it was silently truncated at its first NUL: a 67-byte PNG came
# back 8 bytes (spinel-llvm E055 trap b). The byte COUNT is the assertion;
# printing the bytes would hide the bug behind the terminal.
P = "sp_io_write_poly_#{Process.pid}.bin"

def maybe(c)
  c ? nil : "AB" + 0.chr + "CD" + 255.chr    # String | nil  => TY_POLY
end

def size_of
  n = File.binread(P).bytesize
  File.delete(P)
  n
end

v = maybe(false)

f = File.open(P, "wb"); f.write(v); f.close
puts size_of                                   # 6
f = File.open(P, "wb"); f.write(v, v); f.close
puts size_of                                   # 12
f = File.open(P, "wb"); f << v; f.close
puts size_of                                   # 6
f = File.open(P, "wb"); f.syswrite(v); f.close
puts size_of                                   # 6
f = File.open(P, "wb"); f.write(maybe(true)); f.close
puts size_of                                   # 0   nil writes nothing
f = File.open(P, "wb"); f.write("x"); f.close
puts size_of                                   # 1   TY_STRING arm unchanged
f = File.open(P, "wb"); f.write(42); f.close
puts size_of                                   # 2   non-String stringifies
f = File.open(P, "wb"); f.write(:sym); f.close
puts size_of                                   # 3   unmarked static name
f = File.open(P, "wb"); f.write(1.5); f.close
puts size_of                                   # 3

# the bytes themselves, once, through the workaround-free path
f = File.open(P, "wb"); f.write(v); f.close
puts File.binread(P).bytes.inspect             # [65, 66, 0, 67, 68, 255]
File.delete(P)
