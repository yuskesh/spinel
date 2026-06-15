# Module#/Class#instance_methods(false) returns a real (boxed) poly array of
# symbols, so chained operations like `.map(&:to_s).sort` iterate it correctly
# instead of treating an opaque sym-array box as empty.

module M
  def after_commit_hook; end
  def execute_after_commit; end
end
p M.instance_methods(false).map(&:to_s).sort

class C
  def m1; end
  def m2; end
  attr_reader :r
end
p C.instance_methods(false).map(&:to_s).sort
puts "done"
