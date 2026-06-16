# Method#arity is a compile-time constant derived from the target method's
# parameter shape, read off the method's DefNode. A method reports its required
# argument count, unless an optional positional, a rest parameter, a forwarding
# `...`, or a non-mandatory keyword block makes it variadic, in which case it
# reports -(required + 1). A required keyword counts as one mandatory argument
# and keeps the method's arity fixed; a block parameter never affects arity.

# Zero / required-only.
def none; 1; end
p method(:none).arity

def one(x); x; end
p method(:one).arity

def two(a, b); a + b; end
p method(:two).arity

# Optional parameter -> variadic.
def opt(x, y = 1); x + y; end
p method(:opt).arity

def two_one_opt(a, b, c = 0); a + b + c; end
p method(:two_one_opt).arity

# Rest parameter -> variadic.
def splat(*xs); xs; end
p method(:splat).arity

def req_splat(x, *ys); x; end
p method(:req_splat).arity

# Post-splat required parameter: the requireds before and after the splat both
# count toward the (variadic) required total.
def post_splat(a, *b, c); a; end
p method(:post_splat).arity

# Keyword parameters. A required keyword is one mandatory argument (fixed
# arity); an optional keyword or keyword-rest with no required keyword is
# variadic.
def opt_kw(a, b: 1); a; end
p method(:opt_kw).arity

def req_kw(a, b:); a; end
p method(:req_kw).arity

def mix_kw(a, b:, c: 1); a; end
p method(:mix_kw).arity

def kw_rest(a, **kw); a; end
p method(:kw_rest).arity

def req_kw_rest(a, b:, **kw); a; end
p method(:req_kw_rest).arity

def kw_only_opt(b: 1); b; end
p method(:kw_only_opt).arity

def kw_only_req(b:); b; end
p method(:kw_only_req).arity

# Block parameter does not affect arity.
def with_blk(a, &blk); a; end
p method(:with_blk).arity

# Through a local binding.
adder = method(:two)
p adder.arity

# Instance method via recv.method(:name).
class Calc
  def scale(value, factor); value * factor; end
end
p Calc.new.method(:scale).arity
