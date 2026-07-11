# Array negative-argument guards and nil returns: slice with a negative
# length, sample(-n), Array.new(-n), fetch with a block / CRuby's IndexError
# message, and first/last on an empty literal.
def sl(a, i, n); a[i, n]; end
def sm(a, n); a.sample(n); end
def mk(n); Array.new(n); end
def mkf(n); Array.new(n, 7); end
def fch(a, i); a.fetch(i); end
def fst(a); a.first; end
def lst(a); a.last; end

p sl([1, 2, 3, 4], 0, -1)      # nil
p sl([1, 2, 3, 4], 1, 2)       # [2, 3]
p sl([1, 2, 3, 4], -2, 5)      # [3, 4]
p sl(["a", "b"], 0, -9)        # nil
begin
  sm([1, 2], -1)
rescue ArgumentError => e
  puts "#{e.class}: #{e.message}"
end
p sm([5], 1)                   # [5]
begin
  mk(-1)
rescue ArgumentError => e
  puts "#{e.class}: #{e.message}"
end
begin
  mkf(-3)
rescue ArgumentError => e
  puts "#{e.class}: #{e.message}"
end
p mk(2)                        # [nil, nil]
p [1, 2, 3].fetch(9) { |i| i * 10 }    # 90
p [1, 2, 3].fetch(1) { |i| i * 10 }    # 2
p [1, 2, 3].fetch(-1) { |i| i }        # 3
p [1, 2, 3].fetch(-9) { |i| i }        # -9
begin
  fch([1, 2], 5)
rescue IndexError => e
  puts e.message               # index 5 outside of array bounds: -2...2
end
begin
  fch([1, 2], -3)
rescue IndexError => e
  puts e.message               # index -3 outside of array bounds: -2...2
end
p fst([])                      # nil
p lst([])                      # nil
p fst([4, 5])                  # 4
