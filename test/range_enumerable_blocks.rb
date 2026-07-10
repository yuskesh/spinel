# Block-taking Enumerable methods on an integer Range materialize through
# the array redispatch: sum with a block (it was silently ignored),
# partition, each_with_index, sort_by, chunk_while, and inject/reduce
# block forms.
p (1..3).sum { |x| x * 2 }
x = (1..6).partition(&:even?)
p x
(10..12).each_with_index { |v, i| puts "#{i}:#{v}" }
p (1..5).sort_by { |x| -x }
p (1..5).chunk_while { |a, b| b == a + 1 }.to_a
p (1..4).inject(10) { |s, x| s + x }
p (1..4).reduce(0) { |s, x| s + x }
p (1..4).inject(:+)
p (1..3).sum
