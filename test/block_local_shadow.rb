# A block-local declaration (`|x; sum|`) introduces a fresh variable scoped to
# the block: it shadows any enclosing local of the same name, is reset to nil on
# each block invocation, and must never leak its writes back out.

# shadows an enclosing local -- the outer value survives untouched
sum = 99
[1].each { |x; sum| sum = x }
p sum                                  # 99

# reset to nil each invocation (so `sum || 0` starts from 0 every element)
total = 42
[1, 2, 3].each { |x; total| total = (total || 0) + x }
p total                                # 42

# two block-locals, both shadowing
a = 1
b = 2
[0].each { |z; a, b| a = 7; b = 8 }
p [a, b]                               # [1, 2]

# a nested block-local shadows while the outer block variable stays intact
r = 0
[5].each do |i|
  [9].each { |j; r| r = j }
  r = i
end
p r                                    # 5

# a block-local coexisting with a param of a different name
n = 100
[10, 20].each { |v; n| n = v }
p n                                    # 100
