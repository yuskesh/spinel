# Empty seedless folds are nil; delete's not-found block yields its value;
# flatten! mutates in place; rindex takes a block.
p [].reduce(:+).inspect
p [].inject(:+).inspect
p (1...1).reduce(:+).inspect
p [1, 2].reduce(:+)
p [].reduce(10, :+)
p ["a", "b"].delete("z") { "missing" }
p ["a", "b"].delete("a") { "missing" }
p [1, 2].delete(9) { :nope }
a = [[1], [2, [3]]]
a.flatten!
p a
p [1, 2, 3, 2].rindex { |x| x < 3 }
p [1, 2, 3, 2].rindex(2)
