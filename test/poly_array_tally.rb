# Every argument is a genuinely heterogeneous (poly) array, so the parameter
# stays a poly array and Array#tally dispatches through sp_PolyArray_tally.
def tally_of(arr)
  arr.tally
end

p tally_of(["a", "b", "a", 1, 1, 1, :x, "a"])
p tally_of([:red, "red", :red, 7, 7])
p tally_of([true, false, true, nil, true, nil])

counts = tally_of(["red", 1, "red", "red", 1, :red])
puts counts["red"]
puts counts[1]
puts counts[:red]
puts counts.length
