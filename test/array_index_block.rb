# Array#index { |x| cond } finds the first index whose element satisfies the
# block (an alias for #find_index in this form); returns nil on no match.
# Receivers route through a per-kind identity method to exercise the typed path.
def si(x); x; end
def sw(x); x; end

# block form (the gap)
p si([1, 2, 3]).index { |x| x > 1 }
p si([1, 2, 3]).index { |x| x > 9 }
p sw(%w[a bb ccc]).index { |s| s.length == 2 }
p sw(%w[a bb ccc]).index { |s| s.empty? }

# find_index still behaves identically
p si([4, 5, 6]).find_index { |x| x == 5 }

# value form is unchanged (no block)
p si([1, 2, 3]).index(2)
p si([1, 2, 3]).index(9)
p sw(%w[a b c]).index("b")
