# defined? classifies at compile time: composite expressions answer
# "expression" even with undefined operands (the argument is never
# evaluated), any assignment answers "assignment", runtime-provided special
# globals exist without a program write, and kernel builtins are "method".
p defined?(true && false)
p defined?($undefined_g && true)
p defined?(x = 2)
p defined?(@iv = 3)
p defined?($!)
p defined?($~)
p defined?(puts)
p defined?(1 ? 2 : 3)
p defined?(nil_undefined_method_zzz)
