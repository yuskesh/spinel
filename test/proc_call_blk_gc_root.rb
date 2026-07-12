# Regression for the sp_proc_call heap-use-after-free: a method keeping a real
# &blk param (no yield -- the dead Thread.new branch capturing blk forces the
# escaping sp_Proc path) held the proc box only in an untracked C parameter,
# and the call site's hoisted proc temp was unrooted too. A GC inside the
# block body then swept the box and the next blk.call read freed memory.
# Both call shapes are covered: a toplevel def and a module class method
# (they lower the &blk argument through different codegen paths).

def each_i(n, &blk)
  Thread.new { blk.call(0) }.join if n < 0
  i = 0
  while i < n
    blk.call(i)
    i += 1
  end
end

module Parallel
  def self.each_index(n, &blk)
    Thread.new { blk.call(0) }.join if n < 0
    i = 0
    while i < n
      blk.call(i)
      i += 1
    end
  end
end

acc = Array.new(5, 0.0)
each_i(5) do |i|
  GC.start
  acc[i] += 1.5 * i
end
Parallel.each_index(5) do |i|
  GC.start
  acc[i] += 2.5 * i
end
tot = 0.0
i = 0
while i < 5; tot += acc[i]; i += 1; end
puts "tot=#{tot}"
