# Object identity/reflection conformance (KieranP #2281-2286)
a = ->(x) { x }
p a.equal?(a)                       # #2281 Proc equal?
p a.frozen?                         # #2282 Proc frozen?
b = ->(x) { x }
p a.equal?(b)
e = [1].each
p e.equal?(e)                       # Enumerator equal?
p e.frozen?
r = /a/
p r.equal?(r)                       # Regexp equal?
p r.frozen?                         # a Regexp literal is frozen
class C
  def initialize; @n = 5; end
end
o = C.new
p o.object_id.is_a?(Integer)        # #2283 object_id -> stable Integer
p o.object_id == o.object_id
p o.hash == o.hash                  # #2284 hash is stable
p Object.new.is_a?(Object)          # #2286 is_a?(Object)
p Object.new.is_a?(Kernel)
p Enumerable.ancestors              # #2285 module ancestors
p Comparable.ancestors
class MyColl
  include Enumerable
  def each; yield 1; yield 2; end
end
p MyColl.ancestors.include?(Enumerable)
p(Enumerable.superclass) rescue p($!.class)
