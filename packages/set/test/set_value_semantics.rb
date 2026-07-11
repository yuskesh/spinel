# Set value equality, inspect rendering, combinators, membership predicates,
# classify/divide, and the implicit availability of Set (like CRuby's core
# Set, this file deliberately never requires the library).
def eq(a, b); a == b; end
def cls3(s); s.classify { |x| x % 3 }; end
def dv2(s); s.divide { |x| x % 2 }; end

p eq(Set[1, 2, 3], Set[3, 2, 1])          # true
p eq(Set[1, 2], Set[1, 2, 3])             # false
p eq(Set[1, 2, 3], Set[1, 2, 4])          # false
p Set["1", "2"].to_s                      # "Set[\"1\", \"2\"]"
p Set[1, 2].inspect                       # "Set[1, 2]"

a = Set[1, 2, 3]
b = Set[2, 3, 4]
p [(a | b).to_a.sort, (a & b).to_a.sort,
   (a - b).to_a.sort, (a ^ b).to_a.sort]  # [[1, 2, 3, 4], [2, 3], [1], [1, 4]]
p a.subset?(Set[1, 2, 3, 4])              # true
p a.subset?(a)                            # true
p a.proper_subset?(a)                     # false
p a.proper_subset?(Set[1, 2, 3, 4])       # true
p a.superset?(Set[1, 2])                  # true
p Set[1, 2, 3, 4].proper_superset?(a)     # true

p Set[1, 2].disjoint?(Set[3, 4])          # true
p Set[1, 2].disjoint?(Set[2, 3])          # false
p Set[1, 2].intersect?(Set[2, 3])         # true

p cls3(Set[1, 2, 3, 4, 5, 6])  # {1 => Set[1, 4], 2 => Set[2, 5], 0 => Set[3, 6]}
p dv2(Set[1, 2, 3, 4])         # Set[Set[1, 3], Set[2, 4]]

# init block and in-place block methods
sib = Set.new([1, 2, 3]) { |x| x * 2 }
p sib.to_a.sort
smb = Set[1, 2, 3]
smb.map! { |x| x % 2 }
p smb.to_a.sort
scb = Set[1, 2]
scb.collect! { |x| x + 1 }
p scb.to_a.sort
ssel = Set[1, 2, 3]
rs1 = ssel.select! { |x| x > 1 }
p rs1 ? rs1.to_a.sort : nil
sun = Set[1, 2, 3]
p(sun.select! { |x| x > 0 })
srj = Set[1, 2, 3]
p(srj.reject! { |x| x > 5 })
p srj.delete_if { |x| x > 2 }.to_a.sort
p srj.keep_if { |x| x == 1 }.to_a
sfl = Set[1, 2, 3]
rf1 = sfl.filter! { |x| x.odd? }
p rf1 ? rf1.to_a.sort : nil
