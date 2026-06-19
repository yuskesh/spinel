# matz/spinel#1481: inject/reduce over each.with_index / each_with_index
puts [10, 20, 30].each.with_index.inject(0) { |acc, (val, idx)| acc + idx }       # 3
chars = [1, 2, 3]
puts chars.each.with_index.inject(0) { |sum, (c, k)| sum + c * (26 ** k) }         # 2081 (base26)
puts %w[a b c].each.with_index.inject("") { |str, (ch, n)| str + "#{ch}#{n}" }     # a0b1c2
puts [10, 20].each.with_index.inject(0) { |t, pr| t + pr[0] + pr[1] }              # 31
puts [5, 6, 7].each.with_index.reduce(100) { |r, (e, m)| r + e - m }               # 115
puts [10, 20].each.with_index(10).inject(0) { |o, (x, w)| o + w }                  # 21
puts [1, 2, 3].each_with_index.inject(0) { |q, (y, z)| q + z }                     # 3
