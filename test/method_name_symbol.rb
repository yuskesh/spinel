# Method#name returns a Symbol (not a String), matching CRuby. The method
# object is also routed through a param so the `.name` runs on a non-folded
# TY_METHOD value.
class Calc
  def add(a, b); a + b; end
  def noarg; 0; end
  def +(other); 1; end
end
c = Calc.new

m = c.method(:add)
p m.name                  # :add
puts m.name.inspect       # :add
puts m.name == :add       # true
puts m.name.class         # Symbol
puts m.name.to_s          # add

# no-arg method
puts c.method(:noarg).name.inspect   # :noarg

# operator method name
p c.method(:+).name       # :+

# routed through a method param to defeat constant-folding
def id(x); x; end
p id(c.method(:add)).name # :add

# usable as a case/when scrutinee against symbol literals
case m.name
when :add then puts "is add"
else puts "other"
end
