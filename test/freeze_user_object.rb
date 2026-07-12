# freeze / frozen? on user instances: the bareword self-freeze idiom sets
# real state, frozen? reads it back, and mutation after freeze raises
# FrozenError with CRuby's message (address-normalized for comparison).
def norm(e) = "#{e.class}: #{e.message.sub(/0x[0-9a-f]+/, '0xADDR')}"

class Sealed
  attr_reader :x
  attr_writer :x
  def initialize
    @x = 1
    @tag = "s"
    freeze
  end
  def poke = (@x = 2)
  def poke_expr = (y = (@x = 3); y)
end

o = Sealed.new
p o.frozen?
p o.x
begin
  o.poke
rescue => e
  puts norm(e)
end
begin
  o.poke_expr
rescue => e
  puts norm(e)
end
begin
  o.x = 5
rescue => e
  puts norm(e)
end
p o.x

class Lazy
  attr_accessor :v
  def initialize = (@v = 10)
  def seal = freeze
  def sealed? = frozen?
end

l = Lazy.new
p l.sealed?
l.v = 11
p l.v
l.seal
p l.sealed?
p l.frozen?
begin
  l.v = 12
rescue => e
  puts norm(e)
end
p l.v

class OwnFreeze
  attr_reader :log
  def initialize = (@log = "clean")
  def freeze = (@log = "custom"; self)
end

f = OwnFreeze.new
f.freeze
p f.log
p f.frozen?

class Never
  attr_accessor :n
  def initialize = (@n = 0)
end
nv = Never.new
nv.n = 7
p nv.n
