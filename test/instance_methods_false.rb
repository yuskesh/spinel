# `Klass.instance_methods(false)` const-folds to a literal symbol array
# of the class/module's own (non-inherited) instance methods. The set
# matches method_defined?(name, false): the method table plus attr
# readers and `=`-suffixed writers. The set is public + protected;
# private defs are excluded (matching CRuby). The no-arg / `true` forms
# stay unfolded.
class Foo
  attr_reader :r
  attr_writer :w
  attr_accessor :rw
  def hello; end
  def world(a, b); end
  private
  def secret; end
end

ms = Foo.instance_methods(false)
puts ms.class
puts ms.include?(:hello)
puts ms.include?(:world)
puts ms.include?(:r)
puts ms.include?(:w=)
puts ms.include?(:rw)
puts ms.include?(:rw=)
puts ms.include?(:secret)
puts ms.include?(:nope)

module M
  def self.a; end
  def b; end
  def c(x); end
end

mm = M.instance_methods(false)
puts mm.include?(:b)
puts mm.include?(:c)
puts mm.include?(:a)
puts mm.length

class Empty
end
puts Empty.instance_methods(false).inspect
