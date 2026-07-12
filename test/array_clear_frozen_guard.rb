# Array#clear on a frozen array raises FrozenError with the CRuby message
# (including the inspected contents); a non-frozen array clears normally.
def cl(a); a.clear; a; end
begin
  cl([1, 2].freeze)
rescue FrozenError => e
  puts e.class
  puts e.message
end
arr = [10, 20, 30]
cl(arr)
p arr
