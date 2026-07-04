# Array#eql? compares element-wise, like ==, including on a method-chain result.
def s(x); x; end
p s([2, 1]).sort.eql?([1, 2])
p [1, 2].eql?([1, 2])
p [1, 2].eql?([1, 3])
p [1, 2].eql?([1, 2, 3])
p s([1, 2]).eql?([1, 2])
p s([1, 2]).eql?([9, 9])
