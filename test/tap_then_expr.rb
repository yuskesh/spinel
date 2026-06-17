# Object#tap and Object#then/#yield_self in expression position. tap runs the
# block for a side effect and yields the (unchanged) receiver; then yields the
# block's value. Both work across receiver types, not just a bare statement.
p(5.tap { |x| puts x })             # int receiver, returns 5
p("hi".tap { |s| puts s.upcase })   # string receiver, returns "hi"
p([1, 2, 3].tap { |a| a.push(4) })  # array literal, mutated then returned

p(5.then { |x| x * 2 })             # then yields the block value
p("hi".then { |s| s.upcase })
p([1, 2, 3].then { |a| a.sum })

# chained into another call
doubled = [1, 2, 3].map { |x| x * 2 }.tap { |a| a.push(99) }
p doubled

# tap as a statement still works (mutates in place)
nums = [3, 1, 2]
nums.tap { |a| a.sort! }
p nums
