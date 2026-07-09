# Enumerator#feed and StopIteration#result -- the two halves of the generator
# resume protocol. #feed sets the value the generator's next `y.yield` returns
# (raising TypeError if set twice before a #next); when the generator body runs
# off the end, its return value is carried as StopIteration#result.
#
# Note: for a materialized enumerator (arr.each) CRuby's StopIteration#result is
# the receiver; Spinel reports nil there. Only the fiber-backed generator, whose
# body value is well-defined, carries a result -- that is what is asserted here.

# --- #feed: y.yield returns the fed value ---
g = Enumerator.new { |y| got = y.yield(10); y << (got * 2) }
p g.next          # 10
g.feed(5)
p g.next          # 10  (== 5 * 2)

# with no feed, y.yield returns nil
h = Enumerator.new { |y| a = y.yield(1); p a; y << 2 }
h.next            # yields 1
h.next            # a is nil -> prints nil, then yields 2

# #feed returns nil
k = Enumerator.new { |y| y.yield(1) }
k.next
p k.feed(:x)      # nil

# feeding twice before #next raises TypeError
m = Enumerator.new { |y| y.yield(1); y.yield(2) }
m.next
m.feed(:a)
begin
  m.feed(:b)
rescue TypeError => te
  p te.message
end

# --- StopIteration#result: the generator body's return value ---
en = Enumerator.new { |y| y << 1; y << 2; :finished }
begin
  en.next; en.next; en.next
rescue StopIteration => ex
  p ex.result     # :finished
end

# a numeric terminal value, and the same result on a repeated past-end #next
n = Enumerator.new { |y| y << 10; 99 }
n.next
begin; n.next; rescue StopIteration => ex; p ex.result; end   # 99
begin; n.next; rescue StopIteration => ex; p ex.result; end   # 99 (cached)

# rewind clears any pending feed and the captured result
r = Enumerator.new { |y| got = y.yield(1); y << (got.nil? ? -1 : got) }
r.feed(7)
p r.next          # 1
r.rewind
p r.next          # 1 (fresh run)
p r.next          # -1 (feed was cleared by rewind)

# a generator whose LAST statement is y.yield: #result is that yield's return
# value -- the fed value, or nil if unfed -- not forced to nil like a `<<`
t = Enumerator.new { |y| y.yield(1) }
t.next
begin; t.next; rescue StopIteration => ex; p ex.result; end   # nil (unfed)
w = Enumerator.new { |y| y.yield(1) }
w.next
w.feed(:z)
begin; w.next; rescue StopIteration => ex; p ex.result; end   # :z (fed)
