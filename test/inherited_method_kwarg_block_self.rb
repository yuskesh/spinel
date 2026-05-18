# Two fixes from the roundhouse controller-test C-compile cascade
# (issue #572 follow-up):
#
# 1. When `def f(x, y = @ivar)` was inferred, the optional-param
#    default's initial type computed BEFORE @ivar's class-context
#    was known fell to "int", so a later "first class with the
#    called surface" walk picked the WRONG sibling class.
#    Fix: re-run infer_type on the default expression with the
#    method's owning class scope before the class_has_all_methods
#    heuristic. The roundhouse canary was
#    `assert_response(expected, response = @__response)` against
#    `ActionController_Base` (first sibling defining `status` /
#    `body`) instead of `ActionResponse`.
#
# 2. Inlined yield bodies emitted bare `self` for inherited-method
#    calls inside the body. The enclosing function's C `self` is
#    the subclass pointer; calls to ancestor methods need a cast.
#    Fix: wrap @self_override in a cast to the callee's class so
#    subsequent `sp_<owner>_<m>(self, ...)` calls receive a typed
#    pointer.

class Foo
  attr_accessor :status, :body
  def initialize
    @status = 100
    @body = "Foo"
  end
end

class Bar
  attr_accessor :status, :body
  def initialize
    @status = 200
    @body = "Bar"
  end
end

class Outer
  def initialize
    @resp = Bar.new
  end

  # Without fix #1, `r` was typed Foo (first sibling defining
  # .status/.body) so the caller had to pass a Foo. With the fix,
  # `r` resolves from the default's actual type (Bar via @resp).
  def check(expected, r = @resp)
    raise "got status=#{r.status}, expected=#{expected}" unless r.status == expected
    raise "got body=#{r.body}" unless r.body == "Bar"
  end

  # Without fix #2, the inlined-yield body of `helper` emitted
  # calls to inherited methods with bare `self`, mismatching the
  # enclosing C function's `self` type.
  def helper(label)
    yield
    puts label
  end

  def run
    helper("from-outer") do
      check(200)
    end
  end
end

Outer.new.run
puts "OK"
