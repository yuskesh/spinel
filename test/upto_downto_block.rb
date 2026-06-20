# upto/downto with a block: the body must run whether or not the block declares
# a parameter. Regression: a no-param block (`1.upto(5) { h += 1 }`) dropped its
# body entirely (printed 0). A declared param is rebound each iteration, so
# mutating it inside the block does not perturb the loop (CRuby semantics).
h = 0
1.upto(5) { h += 1 }
puts h                      # 5

i = 0
1.upto(5) { |n| i += n }
puts i                      # 15

c = 0
5.downto(1) { c += 1 }
puts c                      # 5

s = 0
1.upto(3) { |n| n += 100; s += n }
puts s                      # 306

d = 0
9.downto(7) { |n| d += n }
puts d                      # 24
