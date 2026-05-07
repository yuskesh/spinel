# Statement-form `@x OP= v` for the bitwise / shift / multiplicative
# operators. The pre-fix codegen only handled `+=` and `-=`; every
# other op-assign was silently dropped, so `@a &= 0x80` left @a
# untouched. Walks all the operators that map onto C `OP=` directly.

class C
  attr_reader :a
  def initialize; @a = 0xff; end
  def m_amp; @a &= 0x80; end
  def m_or;  @a |= 0x01; end
  def m_xor; @a ^= 0xff; end
  def m_lsh; @a <<= 1; end
  def m_rsh; @a >>= 1; end
  def m_mul; @a *= 2; end
end

c = C.new; c.m_amp; puts "amp: #{c.a}"
c = C.new; c.m_or;  puts "or: #{c.a}"
c = C.new; c.m_xor; puts "xor: #{c.a}"
c = C.new; c.m_lsh; puts "lsh: #{c.a}"
c = C.new; c.m_rsh; puts "rsh: #{c.a}"
c = C.new; c.m_mul; puts "mul: #{c.a}"
