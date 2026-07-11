# Kernel#display, String clone(freeze:), equal?/eql? on nil/bool/symbol,
# and instance_variable_defined? with string/symbol names.
5.display
puts
"str".display
puts
s = "x".freeze.clone(freeze: false)
s << "y"
p s
p(nil.equal?(nil))
p(nil.equal?(0))
p(true.equal?(true))
p(true.equal?(false))
p(false.equal?(false))
p(:a.equal?(:a))
p(:a.equal?(:b))
class C; def initialize; @x = 7; end; end
o = C.new
p o.instance_variable_defined?('@x')
p o.instance_variable_defined?(:@x)
p o.instance_variable_defined?('@nope')
