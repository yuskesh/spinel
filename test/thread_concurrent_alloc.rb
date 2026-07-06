# Concurrent allocation from multiple threads must be thread-safe and
# deterministic. Each worker thread allocates a fresh Array per work item
# (exercising sp_gc_alloc + the container growth paths that adjust the GC
# byte counters) while its siblings do the same; the shared sp_gc_bytes /
# pool free-list state used to race at N>1 workers, skewing GC triggers and
# corrupting the heap. The final sum is a pure function of the inputs, so
# any run-to-run variation means allocation state was corrupted.
#
# The threads write acc at DISJOINT indices (thread w owns i % nt == w) into
# a preallocated Float array that never grows during the run: each slot is a
# distinct memory location, so the writes are race-free by construction --
# this is the documented "safe subset" pattern, and it mirrors how the race
# was hit in the field. Allocation is the only shared state exercised.
def alloc_work(i)
  a = Array.new(8, 0.0)
  j = 0
  while j < 8; a[j] = i * 0.5 + j; j += 1; end
  s = 0.0
  j = 0
  while j < 8; s += a[j]; j += 1; end
  s
end

n = 2000
acc = Array.new(n, 0.0)
nt = 4
ws = []
w = 0
while w < nt
  ws << Thread.new(w) { |wid| i = wid; while i < n; acc[i] = alloc_work(i); i += nt; end }
  w += 1
end
ws.each { |t| t.join }
puts "sum=#{acc.reduce(0.0) { |a, b| a + b }}"
