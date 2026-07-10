# def m((a, b)) destructures an array argument across the parenthesized
# parameter, like block parameters do.
def m((a, b))
  a + b
end
p m([1, 2])
x = m([10, 20])
p x
def mixed(pre, (a, b), post)
  "#{pre}:#{a}#{b}:#{post}"
end
p mixed(1, [2, 3], 4)
def rest3((a, *r))
  [a, r]
end
p rest3([1, 2, 3])
