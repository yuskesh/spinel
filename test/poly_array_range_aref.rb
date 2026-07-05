# Range indexing on a poly (mixed-element) array: arr[a..b] / arr[a...b]
# returns a subarray, like the typed-array receivers already did. Doom's WAD
# reader slices its directory (a poly array of entries) between two markers:
# `@directory[start_idx + 1...end_idx]`.
arr = []
arr << 1
arr << "two"
arr << :three
arr << 4.5
arr << [5]

i = arr.index { |e| e == 1 }
j = arr.index { |e| e == 4.5 }
raise "missing marker" unless i && j

p arr[i + 1...j]
p arr[i..j]
p arr[1..2]
p arr[2..]
p arr[1...1]
p arr[-3..-2]
