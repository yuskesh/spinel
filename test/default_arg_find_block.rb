# find / detect { |x| cond } used as a parameter default. The default is
# evaluated at the call site, where the block's local variable has no top-level
# declaration -- previously this emitted C referencing an undeclared `lv_n`.
A = [1, 2, 3, 4]

def kw(x: A.find { |n| n > 2 })
  x
end
p kw           # 3
p kw(x: 99)    # 99 (default not evaluated when the arg is given)
p kw           # 3 (a second defaulting call in the same scope)

def kwd(y: A.detect { |n| n > 10 })
  y
end
p kwd          # nil (no element matches)

def opt(z = A.find { |n| n == 4 })
  z
end
p opt          # 4
