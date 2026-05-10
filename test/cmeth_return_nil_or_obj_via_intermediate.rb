# Issue #411. A class method that initializes a local to `nil`,
# conditionally reassigns it through an intermediate variable, and
# returns the local emitted with return type `mrb_int` even though
# the local itself was correctly typed `sp_Foo *`:
#
#   static mrb_int sp_Bar_cls_find(void) {
#       sp_Foo * lv_result = NULL;
#       sp_Foo * lv_instance = NULL;
#       lv_result = NULL;
#       lv_instance = sp_Foo_new();
#       lv_result = lv_instance;
#       return lv_result;       // sp_Foo * -> mrb_int — fails
#   }
#
# Root cause: `infer_all_returns`'s cmeth branch ran a single
# `scan_locals` pass before calling `infer_body_return`. The first
# pass typed `result` as "nil" (only the initial `result = nil`
# was visible), so the tail expression `result` inferred as nil
# and `unify_return_type` collapsed the cmeth's return to "nil"
# -> `mrb_int`. The local-decl precompute later upgraded the
# local correctly via `refine_method_body_locals`'s 2-pass logic
# ("nil + nullable_pointer_type -> obj_<C>?"), but by that point
# `@cls_cmeth_returns` was already pinned at "nil".
#
# Fix: replace the single-pass scan_locals in
# `infer_all_returns`'s cmeth branch with a
# `refine_method_body_locals` call so the multi-pass refinement
# runs before `infer_body_return` reads the tail.
#
# Coverage:
#   - the canonical Rails-style `_adapter_find_by_id` shape with
#     intermediate variable (the issue's repro),
#   - same shape returning the local directly through an `if/else`
#     branch (no intermediate, regression check that the existing
#     direct path still works),
#   - the simple `nil`-fallback flow control (`return nil unless
#     ...`) to confirm the unify-with-nil tail still resolves
#     correctly to `obj_<C>?`.

class Foo
  attr_accessor :tag
  attr_accessor :flag
  def initialize(tag)
    @tag = tag
    @flag = 0
  end
end

class Bar
  def self.find(t)
    result = nil
    if t == "yes"
      instance = Foo.new("found")
      result = instance
    end
    result
  end

  def self.find_direct(t)
    result = nil
    if t == "yes"
      result = Foo.new("direct")
    end
    result
  end

  def self.maybe(t)
    return nil unless t == "yes"
    Foo.new("maybe")
  end
end

x = Bar.find("yes")
puts x.nil? ? "nil" : x.tag       # found

y = Bar.find("no")
puts y.nil? ? "nil" : y.tag       # nil

a = Bar.find_direct("yes")
puts a.nil? ? "nil" : a.tag       # direct

b = Bar.find_direct("no")
puts b.nil? ? "nil" : b.tag       # nil

c = Bar.maybe("yes")
puts c.nil? ? "nil" : c.tag       # maybe

d = Bar.maybe("no")
puts d.nil? ? "nil" : d.tag       # nil
