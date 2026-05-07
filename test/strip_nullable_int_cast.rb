# `compile_typed_call_args` casts an int arg to an object-pointer
# parameter via `(sp_<base> *)<expr>`. The base name was derived
# from the raw param-type string. When the param type is nullable
# (e.g. `obj_Foo?`, set when the writer-scan sees both `Foo.new`
# and `nil` writes), the cast came out as `(sp_Foo? *)expr` and
# gcc rejected the stray `?`.
#
# Repro: a setter-shaped method whose param is widened to
# `obj_Foo?` by mixed Foo + nil writes, then called with an int-
# typed argument (a method that returns int). The int-arg path
# fires the cast.

class Foo; end
class C
  def take(f)
    @f = f
  end
  def get_int; 42; end
end
c = C.new
c.take(Foo.new)
c.take(nil)
c.take(c.get_int)
puts "ok"
