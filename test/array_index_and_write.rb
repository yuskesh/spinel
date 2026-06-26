# arr[i] &&= v across array element kinds (IndexAndWriteNode, statement + expr).
# &&= writes only when the slot is already truthy/present. Per-type helpers keep
# each receiver's concrete array type (a shared helper would widen to poly).
def si(x); x; end
def sp(x); x; end
def sg(x); x; end

# int array: present slot overwritten, out-of-bounds slot left alone
a = si([1, 2, 3])
a[0] &&= 9          # present -> 9
a[9] &&= 5          # absent  -> no write
p a

# poly array: truthy slot overwritten, nil slot left alone
q = sp([1, "y", nil])
q[0] &&= 7          # truthy -> 7
q[2] &&= 8          # nil    -> no write
p q

# string array: present slot overwritten
g = sg(["x", "y"])
g[1] &&= "z"
p g

# expression position: yields the resulting slot value
r = (a[0] &&= 4)    # present 9 -> 4, yields 4
p [r, a]
