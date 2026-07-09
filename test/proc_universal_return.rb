# Universal boxed proc return (CRuby's uniform VALUE proc ABI): a first-class
# proc publishes its result through one boxed return channel regardless of the
# value's static type, so a proc reached through a type-erased path -- stored in
# an ivar, forwarded, returned from a method -- yields the right value for every
# return kind. Direct monomorphic calls are unchanged.

# Direct calls across every return kind.
puts((-> { 42 }).call)
puts((-> { "hi" }).call)
p((-> { [1, 2, 3] }).call)
puts((-> { 3.5 }).call)
p((-> { :sym }).call)
p((-> { { a: 1 } }).call)
p((-> { (1..3) }).call)
p((-> { true }).call)

# Explicit return of each kind (interior + tail return routing through the slot).
f = lambda { |n| return "neg" if n.negative?; n * 2 }
puts f.call(-1)
puts f.call(5)

# A proc stored in an ivar and called back (the escaped path the old escape
# analysis had to special-case; now every proc rides the boxed channel).
class Holder
  def store(&blk)
    @blk = blk
  end

  def run(x)
    @blk.call(x)
  end
end
h = Holder.new
h.store { |x| x * 3 }
puts h.run(14)

# A proc returned across a method boundary, then called by the caller.
def make_adder(n)
  ->(x) { x + n }
end
puts make_adder(100).call(5)

# Composition and curry thread the boxed channel between stages.
inc = ->(x) { x + 1 }
dbl = ->(x) { x * 2 }
puts (inc >> dbl).call(10)
puts (inc << dbl).call(10)

# `next`/`break <expr>` carry a value out of a proc too, so a proc whose
# next/break value type differs from its tail must widen to the boxed channel
# (else the .call site trusts the tail's scalar type and mis-unboxes the other
# path). Both direct and through the type-erased ivar path.
nx = ->(x) { next "s" if x > 0; 42 }
p nx.call(3)      # "s"
p nx.call(-1)     # 42
bk = ->(x) { break [x, x] if x > 0; 7 }
p bk.call(2)      # [2, 2]
p bk.call(-1)     # 7

relay = Holder.new
relay.store { |x| next :sym if x > 0; 9 }
p relay.run(1)    # :sym
p relay.run(-1)   # 9
