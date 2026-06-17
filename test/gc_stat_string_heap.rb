# GC.stat surfaces the string heap (str_bytes / str_count). Spinel allocates
# string data on a separate malloc'd heap (sp_str_heap) that is deliberately
# excluded from the object-heap `bytes` figure (see sp_str_alloc), so a
# string-heavy workload can hold gigabytes that `bytes` never reflects.
# Retaining 1000 strings makes str_count >= 1000 and str_bytes > 0.
arr = []
i = 0
while i < 1000
  arr.push(i.to_s)
  i += 1
end
g = GC.stat
puts "str_count >= 1000: " + (g["str_count"] >= 1000 ? "yes" : "no")
puts "str_bytes > 0: " + (g["str_bytes"] > 0 ? "yes" : "no")
puts "retained: " + arr.length.to_s
