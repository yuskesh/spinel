p((1..100).bsearch { |x| x*x >= 50 ? x*x - 50 : nil })
p((0..100).bsearch { |x| 100 - x*x })
p((1..10).bsearch { |x| x >= 4 })
p((1..100).bsearch { |x| x < 8 ? 1 : (x == 8 ? 0 : -1) })
p [1, 3, 7, 9, 12].bsearch { |x| x < 9 ? nil : (x == 9 ? 0 : -1) }
p [1, 3, 7, 9, 12].bsearch { |x| x - 7 }
p((1..10).bsearch { |x| x - 8 })
