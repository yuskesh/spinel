class Box
  def initialize
    @v = 0
    @name = "box"
  end
  def v = @v
end

# receiver via a method param exercises a non-folded object receiver
def setget(b)
  b.instance_variable_set(:@v, 99)
  b.instance_variable_get(:@v)
end

b = Box.new
b.instance_variable_set(:@v, 42)
puts b.instance_variable_get(:@v)
puts b.v
puts setget(Box.new)

# instance_variable_set evaluates to the assigned value
x = b.instance_variable_set(:@v, 7)
puts x
puts b.v

# a String name resolves the same as a symbol
puts b.instance_variable_get("@name")

# an undefined but valid @-name reads as nil (CRuby returns nil for an unset ivar)
p b.instance_variable_get(:@missing)

# a name without a leading @ raises NameError at runtime
begin; b.instance_variable_get(:x); rescue => e; puts "#{e.class}: #{e.message}"; end
begin; b.instance_variable_set(:y, 1); rescue => e; puts "#{e.class}: #{e.message}"; end
begin; b.instance_variable_get("z"); rescue => e; puts "#{e.class}: #{e.message}"; end
