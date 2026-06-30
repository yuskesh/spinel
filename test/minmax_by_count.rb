# Array#min_by(n) / max_by(n) with a count return the n smallest / largest
# elements by the block's key, as a generic Array: min_by ascending, max_by
# descending. Receivers are routed through per-type identity methods to defeat
# constant folding. Keys are distinct so ordering is unambiguous (Ruby's count
# form uses an internal heap with unspecified order for tied keys).
def ss(x); x; end
def si(x); x; end

p ss(%w[bbbb a ccc dd]).max_by(2, &:length)   # ["bbbb", "ccc"]
p ss(%w[bbbb a ccc dd]).min_by(2, &:length)   # ["a", "dd"]
p si([3, 1, 4, 1, 5, 9, 2]).max_by(3) { |n| n }   # [9, 5, 4]
p si([3, 1, 4, 6, 5, 9, 2]).min_by(3) { |n| -n }  # [9, 6, 5]

# count larger than the array, and zero count.
p si([1, 2, 3]).max_by(5) { |n| n }    # [3, 2, 1]
p si([1, 2, 3]).min_by(0) { |n| n }    # []

# the no-count form still returns a single element.
p ss(%w[bb a ccc dd]).max_by(&:length)   # "ccc"
p si([3, 1, 2]).min_by { |n| n }         # 1

# a negative count raises ArgumentError, as in CRuby (not an empty array).
begin
  si([1, 2, 3]).max_by(-1) { |n| n }
rescue ArgumentError => e
  p e.message
end                                       # "negative size"
