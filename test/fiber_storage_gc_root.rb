# Regression: sp_fiber_root.storage must survive GC. sp_fiber_root
# is a static (not sp_gc_alloc'd) so its scan never runs via the
# heap walker. Without explicit marking in sp_re_mark_globals, the
# SymPolyHash allocated by `Fiber[:k] = v` at top level gets
# prematurely collected on the next cycle.
#
# Force GC by allocating many sp_gc_alloc'd objects (arrays of
# strings, each becoming garbage on the next iteration) to push
# sp_gc_bytes past the 256KB threshold. Then verify the storage
# value still reads back.

Fiber[:answer] = 42

i = 0
while i < 10000
  # Each iteration allocates a fresh PolyArray + a String element;
  # both are GC-tracked (sp_gc_alloc), so this drives sp_gc_bytes
  # past the auto-collect threshold within the loop.
  garbage = ["garbage_" + i.to_s, "more_" + i.to_s, "still_" + i.to_s]
  i = i + 1
end

# If sp_fiber_root.storage wasn't marked, the SymPolyHash holding
# :answer would have been freed and read would return nil.
v = Fiber[:answer]
if v == 42
  puts "survived"
else
  puts "lost"
end
