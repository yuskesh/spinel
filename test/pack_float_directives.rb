# Array#pack on a Float array and the float/double directives (#pack_unpack_directives):
# D/d/F/f native, E/e little-endian, G/g big-endian. Previously a TY_FLOAT_ARRAY
# receiver had no codegen arm at all ("unsupported call: ... `pack` recv=.../ty18"),
# and String#unpack silently skipped the float directives (fsize 0), so
# [1.5].pack("G") was a compile error and unpack("G") decoded to [].

# double directives: exact hex of the 8-byte IEEE-754 encoding
puts [1.5].pack("G").unpack1("H*")
puts [1.5].pack("E").unpack1("H*")
puts [1.5].pack("D").unpack1("H*")
puts [1.5].pack("d").unpack1("H*")

# single precision (a value exactly representable in float32)
puts [-2.5].pack("g").unpack1("H*")
puts [-2.5].pack("e").unpack1("H*")
puts [-2.5].pack("f").unpack1("H*")
puts [-2.5].pack("F").unpack1("H*")

# star count and unpack round-trip
s = [1.5, -0.25, 1024.0].pack("G*")
puts s.bytesize
puts s.unpack("G*").inspect
puts [3.25, 7.5].pack("e2").unpack("e2").inspect

# integer directives truncate the Float toward zero, as CRuby's coercion does
puts [1.5].pack("C*").bytes.inspect
puts [-3.75, 200.9].pack("c2").unpack("c2").inspect

# an Int-array receiver promotes to double under a float directive
puts [1, 2].pack("G2").unpack("G*").inspect

# poly (mixed int/float) receiver
mixed = [1, 2.5]
puts mixed.pack("E2").unpack("E*").inspect

# an explicit count past the end pads with nil, like the integer directives
puts "\x3F\xF8\x00\x00\x00\x00\x00\x00".unpack("GG").inspect

# unpack1 with a literal single float directive types as an unboxed Float
# (an_unpack1_lit_type), so the result participates in float arithmetic
pair = [1.5, 2.25].pack("G2")
v = pair.unpack1("G")
puts (v * 2).inspect
puts pair.unpack1("G", offset: 8).inspect

# NaN / Infinity under an integer directive raise FloatDomainError, and a
# finite double beyond the int64 range wraps modulo 2**64 (CRuby coerces
# through an exact Integer; a plain C cast would be undefined behaviour)
begin
  [Float::NAN].pack("C")
rescue FloatDomainError => e
  puts "nan: #{e}"
end
begin
  [-Float::INFINITY].pack("q")
rescue FloatDomainError => e
  puts "inf: #{e}"
end
puts [2.0e19].pack("Q").unpack1("Q")
puts [-2.0e19].pack("q").unpack1("q")
puts [1.0e300].pack("Q").unpack1("Q")

# unpack1 with a literal float directive on too-short input returns nil
# (not 0.0): sp_str_unpack pads with nil and the unboxing must preserve it
puts "".unpack1("G").inspect
puts "\x00\x00".unpack1("e").inspect
puts [9.5].pack("G").unpack1("G").inspect
