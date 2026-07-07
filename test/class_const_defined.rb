# Class#const_defined? answers at compile time from the flat constant/class
# registry (the same namespace a constant read resolves against).
class Foo
  CONST = 1
end
o = Foo.new
p o.class.const_defined?(:CONST)
p o.class.const_defined?(:NOPE)
p Foo.const_defined?(:CONST)
p Foo.const_defined?("CONST")
p Foo.const_defined?(:Foo)
