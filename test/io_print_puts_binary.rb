# IO#print / IO#puts / Kernel#print / Kernel#puts sized a binary String with
# strlen, so it was silently truncated at its first NUL -- REGARDLESS of the
# inferred type (unlike the IO#write family, whose TY_STRING arm was already
# binary-safe), and IO#puts then read the byte before the first NUL instead of
# the true last byte for its trailing-newline check (spinel-llvm E059 S26,
# fixed alongside as E086). File-backed asserts use the byte COUNT, like
# test/io_write_poly_binary.rb; the stdout-only Kernel paths print the raw
# bytes, which the snapshot compares byte-for-byte.
P = "sp_io_print_puts_bin_#{Process.pid}.bin"

def maybe(c)
  c ? nil : "AB" + 0.chr + "CD" + 255.chr    # String | nil  => TY_POLY
end

def size_of
  n = File.binread(P).bytesize
  File.delete(P)
  n
end

b = "AB" + 0.chr + "CD" + 255.chr            # TY_STRING, 6 bytes
v = maybe(false)

f = File.open(P, "wb"); f.print(b); f.close
puts size_of                                   # 6   TY_STRING print
f = File.open(P, "wb"); f.print(v); f.close
puts size_of                                   # 6   poly print (tag decides)
f = File.open(P, "wb"); f.puts(b); f.close
puts size_of                                   # 7   TY_STRING puts + "\n"
f = File.open(P, "wb"); f.puts(v); f.close
puts size_of                                   # 7   poly puts
f = File.open(P, "wb"); f.puts("AB" + 0.chr + "CD" + "\n"); f.close
puts size_of                                   # 6   newline check reads the TRUE last byte
f = File.open(P, "wb"); f.puts([b, b]); f.close
puts size_of                                   # 14  array flattens, one "\n" each
f = File.open(P, "wb"); f.puts(42); f.close
puts size_of                                   # 3   non-String stringifies, unchanged
f = File.open(P, "wb"); f.print(:sym); f.close
puts size_of                                   # 3   unmarked static name stays strlen
s = "AB".dup; s << 0.chr; s << "CD"
f = File.open(P, "wb"); f.puts(s); f.close
puts size_of                                   # 6   builder handle (0xfd payload)

# the stdout-only lanes, raw bytes into the snapshot
print b
puts b
$stdout.print(b)
$stdout.puts(b)
puts [b, b]
a = [b]
puts(*a)
print(*a)
puts v
print v
puts "tail"
