# Issue #407 case 1: a class/module class method whose name
# collides with a Kernel/Object inferred-type default (e.g.
# `hash`, `to_s` is exempt because it actually works the right
# way already; `inspect`, `methods`, ...) used to lose the
# user-defined return type. `infer_method_name_type`'s arms have
# shape `if mname == "hash"; return "int"; end` regardless of
# receiver, so a `def self.hash(plain) -> String` on a class
# resolved to "int" through the Kernel-side hash hardcode and
# downstream locals widened to mrb_int -- the C compile then
# tripped at the next site that fed the supposed-integer to a
# `const char *` parameter.
#
# Fix: at the top of `infer_method_name_type`, return "" when
# recv is a constant-class-or-module reference whose target
# defines a class method named mname. This defers to the
# receiver-aware resolution that `infer_constant_recv_type` runs
# later (cls_cmethod_return_inherited / `<Mod>_cls_<m>` synth
# lookup), so user-defined cmeths win over the Kernel-name
# defaults exactly when the receiver pins them.
#
# Coverage:
#   - module class method named `hash` returning String,
#   - real-class class method named `methods` returning String,
#   - real-class class method named `to_s` already had the right
#     answer through other paths; `inspect` here exercises the
#     same shape with a fresh name to lock the receiver-aware
#     gate independently.

module Auth
  class Password
    def self.hash(plain)
      "pbkdf2$" + plain
    end
  end
end

stored = Auth::Password.hash("hunter2")
puts stored                               # pbkdf2$hunter2
puts stored.length.to_s                   # 14

class Registry
  def self.methods(prefix)
    "[" + prefix + ":methods]"
  end

  def self.inspect_kind(kind)
    "<Registry inspect: " + kind + ">"
  end
end

names = Registry.methods("public")
puts names                                # [public:methods]
puts names.length.to_s                    # 16

shape = Registry.inspect_kind("module")
puts shape                                # <Registry inspect: module>
