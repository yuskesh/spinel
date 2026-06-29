# frozen_string_literal: true
# Bundled tests:
#   - str_setbyte_frozen_literal
#   - strip_nullable_int_cast

# === str_setbyte_frozen_literal ===
# Spinel adopts `# frozen_string_literal: true` semantics
# globally — every string literal is frozen, mutation requires
# a heap-allocated buffer (`.dup`, `+`, etc.). setbyte on a
# literal raises FrozenError with the same message CRuby uses
# when the pragma is in effect.

# Direct literal recv.
begin
  "abc".setbyte(0, 67)
  puts "no raise (literal)"
rescue FrozenError => e
  puts "literal: " + e.message
end

# LV pointing at a literal.
s = "abc"
begin
  s.setbyte(0, 67)
  puts "no raise (lv-literal): " + s
rescue FrozenError => e
  puts "lv-literal: " + e.message
end

# Dup'd heap string mutates cleanly.
s2 = "abc".dup
s2.setbyte(0, 67)
puts s2   # Cbc

# Concatenation produces a heap buffer too.
s3 = "x" + "y"
s3.setbyte(0, 90)
puts s3   # Zy

# Heap aliasing: setbyte on shared object affects both refs.
s4 = "ab".dup
s5 = s4
s4.setbyte(0, 67)
puts s5   # Cb (shared heap, both see the mutation)

# ivar holding a heap string mutates through method dispatch.
class T_str_setbyte_frozen_literal_Buf
  attr_reader :s
  def initialize
    @s = "abc".dup
  end
  def hit
    @s.setbyte(0, 67)
  end
end
b = T_str_setbyte_frozen_literal_Buf.new
b.hit
puts b.s   # Cbc

# === strip_nullable_int_cast ===
# `compile_typed_call_args` casts an int arg to an object-pointer
# parameter via `(sp_<base> *)<expr>`. The base name was derived
# from the raw param-type string. When the param type is nullable
# (e.g. `obj_Foo?`, set when the writer-scan sees both `Foo.new`
# and `nil` writes), the cast came out as `(sp_Foo? *)expr` and
# gcc rejected the stray `?`.
#
# Repro: a setter-shaped method whose param is widened to
# `obj_Foo?` by mixed Foo + nil writes, then called with an int-
# typed argument (a method that returns int). The int-arg path
# fires the cast.

class Foo; end
class T_strip_nullable_int_cast_C
  def take(f)
    @f = f
  end
  def get_int; 42; end
end
c = T_strip_nullable_int_cast_C.new
c.take(Foo.new)
c.take(nil)
c.take(c.get_int)
puts "ok"

