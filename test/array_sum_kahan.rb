# Array#sum on floats must use Kahan-Babuska-Neumaier compensated
# summation, matching CRuby: a naive left-fold of these values drifts
# (0.1+0.2+0.3 -> 0.6000000000000001), the compensated sum yields 0.6.
puts [0.1, 0.2, 0.3].sum
puts [0.1, 0.2, 0.3].sum(1.0)
puts [1.0, 2.0, 3.0].sum
puts([0.1] * 10 == [0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1])
puts [0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1].sum
puts [1, 2, 3].sum            # integer sum stays exact

# block form promotes to float and compensates too
arr = [0.1, 0.2, 0.3]
puts arr.sum { |x| x * 2 }
puts arr.sum(1.0) { |x| x }
puts [1, 2, 3].sum { |x| x * 2 }
