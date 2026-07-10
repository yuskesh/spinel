# Default Object#to_s / #inspect for user objects, and raising a
# non-exception object. puts obj was an unsupported argument, obj.inspect
# answered the "[]" last-resort, and `raise obj` smuggled the pointer into
# the message slot (a C warning + garbage). Addresses are normalized here;
# the format matches CRuby's #<Name:0x...> / ivar dump.
class Cell
  def initialize(x, y)
    @x = x
    @y = y
    @alive = false
    @tag = nil
    @list = []
    @name = "c1"
  end
end
class Inner
  def initialize; @k = 7; end
end
class Outer
  def initialize; @inner = Inner.new; end
end
class Named
  def initialize; @n = 1; end
  def to_s; "custom"; end
  def inspect; "<custom>"; end
end

norm = ->(s) { s.gsub(/0x[0-9a-f]+/, "0xADDR") }
c = Cell.new(0, 1)
puts norm.call(c.to_s)
puts norm.call(c.inspect)
puts norm.call("interp: #{c}")
puts norm.call(Outer.new.inspect)   # nested object recurses
puts Named.new
p Named.new
# an object read back out of a container is poly-boxed: its .inspect goes
# through sp_poly_inspect's user-object hook, not the typed path (#1875)
cells = {}
cells["0-0"] = Cell.new(2, 3)
puts norm.call(cells["0-0"].inspect)
mixed = [Cell.new(4, 5), "s"]
puts norm.call(mixed[0].inspect)
begin
  raise c
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
