# A materialized enumerator (a blockless builtin iterator) carries its iterated
# receiver, so a past-the-end StopIteration reports it as #result -- matching
# CRuby, where e.g. Array#each returns the array. The receiver is threaded
# through every array/hash constructor (each / each_slice / each_cons /
# each_with_index / each_index / with_index) and the hash pair iterator.
#
# Note: Range#each (lowered to an integer-array snapshot) and String#each_char
# (built directly from items) do not carry the original range/string, so their
# #result is the materialized collection / nil -- a minor documented divergence.

def result_of(en)
  en.next while true
rescue StopIteration => e
  e.result
end

p result_of([1, 2, 3].each)
p result_of([10, 20].each)
p result_of([1, 2, 3, 4].each_slice(2))
p result_of([1, 2, 3, 4].each_cons(2))
p result_of([5, 6, 7].each_with_index)
p result_of([5, 6, 7].each_index)
p result_of({ "a" => 1, "b" => 2 }.each)
p result_of([9, 8, 7].each.with_index)

# a generator's #result is its own body value, not a receiver
g = Enumerator.new { |y| y << 1; :done }
p result_of(g)
