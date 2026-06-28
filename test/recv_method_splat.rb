# Splatting an array into a user-object method call `obj.f(*args)`. Free-function
# splat already worked; this covers the receiver-method dispatch path.
class C
  def add3(a, b, c); a + b + c; end
  def join2(a, b); "#{a}-#{b}"; end
  def opt(a, b = 10, c = 20); a + b + c; end
  def show(a, b, c); [a, b, c].inspect; end
end

o = C.new
nums = [1, 2, 3]
puts o.add3(*nums)            # 6 -- whole-array splat
puts C.new.add3(*nums)       # 6 -- splat on a fresh receiver
rest = [2, 3]
puts o.add3(1, *rest)        # 6 -- leading positional + splat

parts = ["x", "y"]
puts o.join2(*parts)         # x-y -- string elements

puts o.opt(*[1])             # 31 -- optional params fall back to defaults
puts o.opt(*[1, 2])          # 23
puts o.opt(*[1, 2, 3])       # 6

mixed = [1, "two", :three]
puts o.show(*mixed)          # [1, "two", :three] -- mixed/poly elements

# wrong element count raises ArgumentError with CRuby's message
begin
  o.add3(*[1, 2])
rescue ArgumentError => e
  puts "AE: #{e.message}"
end
