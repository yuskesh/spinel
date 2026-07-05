# frozen_string_literal: true
# `fresh_receiver.delete("\0")` must not corrupt the receiver. Before the fix
# the frozen "\0" literal was re-allocated on every call, and that allocation
# could trigger a GC that swept the (unrooted) fresh receiver before delete
# read it -- a use-after-free (doom's `data[8,8].delete("\0")` WAD name parse).
# Here the receiver has no NUL, so delete removes nothing: it still allocates a
# fresh string (delete always does), but the resulting bytes are unchanged.
out = []
8000.times do |i|
  out << "item#{i % 100}".delete("\x00")
end
puts out.length
puts out[0]
puts out.last
bad = 0
out.each_with_index { |s, i| bad += 1 unless s == "item#{i % 100}" }
puts "bad=#{bad}"

# Sibling-operand shape: a fresh string operand sits next to a frozen
# NUL-containing literal. The literal allocates on first evaluation (to fill its
# call-site cache), so subtree_may_allocate must flag it as allocating -- else
# the fresh operand is left unrooted and a GC during that first allocation frees
# it. `a + "pad\0"` keeps "pad" (sp_str_plus stops the literal at its NUL).
def joinit(a)
  a + "pad\x00"
end
res = []
8000.times do |i|
  res << joinit("head#{i}")
end
GC.start
sbad = 0
res.each_with_index { |s, i| sbad += 1 unless s == "head#{i}pad" }
puts "scount=#{res.size} sbad=#{sbad}"
puts res[0]
puts res.last
