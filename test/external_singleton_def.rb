class D; end
def D.go; 42; end
puts D.go

def D.dbl(x); x * 2; end
puts D.dbl(21)

module M; end
def M.wrap; "[" + yield + "]"; end
puts M.wrap { "hi" }

class Box
  def initialize(v); @v = v; end
  def val; @v; end
end
def Box.make(v); Box.new(v); end
puts Box.make(11).val

class Later
  def self.inside; 1; end
end
def Later.outside; inside + 100; end
puts Later.outside
