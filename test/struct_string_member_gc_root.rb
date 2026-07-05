# A single fresh-string struct member must survive the struct's own allocation
# (SP_POOL_NEW can trigger a GC before the member is stored). The constructor
# roots its heap arguments so the collector cannot sweep the name before it is
# assigned, nor read a freed name when it later marks the (live) struct.
Entry = Struct.new(:name)
entries = []
8000.times do |i|
  entries << Entry.new("item#{i}".upcase)
end
GC.start
bad = 0
entries.each_with_index { |e, i| bad += 1 unless e.name == "ITEM#{i}" }
puts "count=#{entries.size} bad=#{bad}"
puts entries[0].name
puts entries[4000].name
puts entries.last.name

# Non-string heap members must be rooted too. A bigint member is a heap object
# but is neither string/poly/object/array/hash, so the old rooting predicate
# skipped it: a fresh bigint temporary could be swept across SP_POOL_NEW's GC.
Big = Struct.new(:n)
bigs = []
8000.times do |i|
  bigs << Big.new(2 ** 70 + i)
end
GC.start
bbad = 0
bigs.each_with_index { |x, i| bbad += 1 unless x.n == 2 ** 70 + i }
puts "bcount=#{bigs.size} bbad=#{bbad}"
puts bigs[0].n
puts bigs.last.n
