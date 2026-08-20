# D-13: the post-fixpoint re-narrow frame also re-derives a TY_POLY return type
# each iteration.  A self-referential poly cycle whose only anchors are int
# (fixed_point below: the return feeds an ivar that feeds the parameter that
# feeds the return) must settle on int; a genuinely heterogeneous return must
# stay poly.  And the frame must leave params settled when it exits, or the
# `is_a?` guard narrowing (which only narrows a local whose type is exactly
# poly) silently stops firing.
class Cyc
  def initialize
    @acc = 0
    @seed = 1
  end

  # the self-loop: div's own result flows back in as its second operand
  def div(a, b)
    return 0 if b == 0
    (a * 2) / b
  end

  def step(n)
    @acc = div(n, @seed)
    @seed = div(@acc + 1, 2)
    @acc
  end

  def run
    i = 0
    while i < 3
      step(i + 1)
      i += 1
    end
    @acc + @seed
  end
end

p Cyc.new.run

# a genuinely heterogeneous return stays poly and still works
class Mixed
  def pick(n)
    return "s" if n == 0
    return [1, 2] if n == 1
    n
  end
end

m = Mixed.new
p m.pick(0)
p m.pick(1)
p m.pick(2)

# the is_a? guard narrowing must survive the frame
def show(v)
  return v if v.is_a?(String)
  return v.to_s if v.is_a?(Integer)
  return "[" + v.length.to_s + "]" if v.is_a?(Array)
  "?"
end

p show("a")
p show(7)
p show([1, 2, 3])
p show(:z)
