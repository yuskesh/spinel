# A method's &block parameter is reachable from a Thread.new / Fiber.new block
# in that method's body. The block runs as an independent closure on its own
# fiber stack, so the method must keep a heap-materialized &blk (not be
# yield-inlined) and the call site must pass the lowered block proc. Previously
# the escape went undetected: the method was inlined and the thread closure
# referenced an undeclared lv_blk (C compile error), or -- for a class/module
# method -- the block was silently dropped at the call site.

# module (class-)method: &blk captured by a Thread.new block
module M
  def self.f(&blk)
    t = Thread.new { blk.call(0) }
    t.join
  end
end
M.f { |i| puts "got #{i}" }            # got 0

# instance method: same capture through a thread
class C
  def run(&blk)
    t = Thread.new { blk.call(7) }
    t.join
  end
end
C.new.run { |i| puts "run #{i}" }      # run 7

# a positional parameter ahead of &blk, forwarded into the thread
module N
  def self.g(prefix, &blk)
    t = Thread.new { blk.call(prefix) }
    t.join
  end
end
N.g("x") { |s| puts "pre #{s}" }       # pre x

# Fiber.new capture of the &blk param
module F
  def self.h(&blk)
    fib = Fiber.new { blk.call(3) }
    fib.resume
  end
end
F.h { |i| puts "fib #{i}" }            # fib 3

# class-method &blk used as a value (assigned to a local, then called): the
# call site must still pass the block proc (was dropped before the fix)
module V
  def self.take(&blk)
    p = blk
    p.call(9)
  end
end
V.take { |i| puts "val #{i}" }         # val 9

# class-method &blk with a value-returning call, summed across two invocations
module S
  def self.calc(&blk)
    blk.call(1) + blk.call(2)
  end
end
puts S.calc { |i| i * 10 }             # 30
