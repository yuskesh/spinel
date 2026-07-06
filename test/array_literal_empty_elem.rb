# An empty container literal as an array element is a non-scalar value: the
# outer literal must become a poly array, not collapse to an int array that
# stores the nested pointer as an integer.
x = [[], 1, 2, 3]
p x
y = [{}, 5]
p y.length
p x[0] == []
