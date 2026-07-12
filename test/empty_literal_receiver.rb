# Methods on a bare empty-array literal receiver: the ntype pin serves
# product/reduce/inject (a literally-empty reduce IS its init, nil
# without one), alongside the flatten/first arms.
p [].product([1, 2])
p [].reduce { |a, b| a + b }
p [].reduce(5) { |a, b| a + b }
p [].inject { |a, b| a + b }
p [].flatten
p [].first
