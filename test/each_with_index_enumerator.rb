# A blockless Array#each_with_index / #each_index returns an external
# Enumerator: each_with_index steps through [element, index] pairs, each_index
# through the indices. #next / #peek / #rewind / #to_a / #size all work over the
# materialized snapshot. The array is routed through a method so it is not
# constant-folded at the call site.

def ewi(a)
  a.each_with_index
end

e = ewi([10, 20, 30])
p e.next
p e.peek
p e.next
p e.next
p e.size
e.rewind
p e.next

# to_a materializes all pairs.
p ewi([1, 2, 3]).to_a

# each_index yields the indices.
def eidx(a)
  a.each_index
end

g = eidx([9, 8, 7, 6])
p g.next
p g.next
p g.to_a

# string-element array pairs.
p %w[a b c].each_with_index.to_a

# StopIteration past the end.
h = ewi([1])
p h.next
begin
  h.next
rescue StopIteration
  puts "stop"
end
