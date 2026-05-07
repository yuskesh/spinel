# `@b = @a &= 0x80` parses as InstanceVariableWriteNode whose value
# is an InstanceVariableOperatorWriteNode. Without an inner-write
# emitter, the `@a &= ...` evaporated and `@b = ...` saw `0` —
# both ivars ended up wrong. Verifies the chained form against the
# parens-rewrite (`@b = (@a = @a & 0x80)`) and the manual two-stmt
# split — all three should produce the same `@a=128 @b=128`.

class C
  attr_reader :a, :b
  def initialize
    @a = 0xff
    @b = 0
  end
  def m_chain
    @b = @a &= 0x80
  end
  def m_paren
    @b = (@a = @a & 0x80)
  end
  def m_split
    @a = @a & 0x80
    @b = @a
  end
end

c1 = C.new; c1.m_chain; puts "chain: a=#{c1.a} b=#{c1.b}"
c2 = C.new; c2.m_paren; puts "paren: a=#{c2.a} b=#{c2.b}"
c3 = C.new; c3.m_split; puts "split: a=#{c3.a} b=#{c3.b}"
