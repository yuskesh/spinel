# String#<< as an expression on a STRBUF-promoted local; Array slice(i)/
# slice(range), pop(n)/shift(n), max(n){cmp}/min(n){cmp}, sum(str){blk};
# blockless cycle(n)/each_entry chains; collect_concat as a flat_map alias.
s = "ab"
t = s << "cd"
p t
p s
u = "x"
p(u << "y" << "z")
p([10, 20, 30, 40].slice(1))
p([10, 20, 30, 40].slice(1..3))
a = [1, 2, 3, 4]
p a.pop(2)
p a
b = [1, 2, 3, 4]
p b.shift(2)
p b
c = [1, :x, "s", 4.0]
p c.pop(2)
p([1, 2, 3, 4].max(2) { |x, y| x <=> y })
p([1, 2, 3, 4].min(2) { |x, y| x <=> y })
p(["a", "b", "c"].sum("x") { |e| e })
p [1, 2].cycle(3).to_a
p [1, 2, 3].each_entry.to_a
p ["a", :b].cycle(2).to_a
p [1, 2].cycle(2).map { |x| x * 10 }
p [1, 2, 3].collect_concat { |x| [x, x * 10] }
p [[1, 2], [3]].collect_concat { |e| e }
