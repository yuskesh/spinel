# Range Enumerable forms: take_while/drop_while blocks, comparator min/max,
# beginless/endless include?/cover?, bsearch find-any vs find-minimum mode,
# and #to_a on runtime-tagged (poly) receivers including nil.

def tw(r); r.take_while { |i| i < 4 }; end
def dw(r); r.drop_while { |i| i < 4 }; end
p tw(1..6)      # [1, 2, 3]
p dw(1..6)      # [4, 5, 6]
p tw(5..8)      # [] (first element fails)
p dw(5..8)      # [5, 6, 7, 8] (nothing dropped)

def mx(r); r.max { |a, b| b <=> a }; end
def mn(r); r.min { |x, y| y <=> x }; end
p mx(1..5)      # 1 (reversed comparator)
p mn(1..5)      # 5
p mx(3..3)      # single element

def inc(r, v); r.include?(v); end
def cov(r, v); r.cover?(v); end
p inc((..5), 1)     # true
p inc((..5), 5)     # true
p inc((..5), 6)     # false
p inc((...5), 5)    # false (exclusive upper)
p inc((1..), 100)   # true
p inc((1..), 0)     # false
p cov((..0), -9)    # true
p inc(1..5, 3)      # bounded ranges keep working
p inc(1..5, 9)      # false

def bs_any(r, t); r.bsearch { |v| t <=> v }; end
def bs_min(r); r.bsearch { |w| w >= 3 }; end
p bs_any(0..100, 42)   # 42 (find-any hit)
p bs_any(0..3, 5)      # nil (target above range)
p bs_any(4..9, 1)      # nil (target below range)
p bs_min(0..10)        # 3 (find-minimum mode unchanged)

def ta(x); x.to_a; end
p ta(nil)               # []
p ta([1, "two", 3])     # identity contents
p ta({a: 1, b: 2})      # [[:a, 1], [:b, 2]]
begin
  ta(5)
rescue NoMethodError => e
  puts e.message        # undefined method 'to_a' for an instance of Integer
end
