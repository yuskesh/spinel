# Unary +@ on a string returns a mutable copy (so a literal can be built up),
# and concat / []= mutate a mutable string local in place (statement position,
# like the existing <</upcase!). -@ returns the string unchanged.

# +"..." yields a mutable string; appends build it up
s = +"abc"
s << "d"
p s
s.concat("ef")
p s
s.concat("g", "h", "i")
p s
s.concat(33)            # an Integer appends its codepoint
p s

# build-in-a-loop starting from +""
acc = +""
3.times { |i| acc << i.to_s }
p acc

# in-place bang methods on a +string
u = +"Hello"
u.upcase!
p u
u.reverse!
p u

# []= single-index assignment (negative index counts from the end; the value
# may be longer than one character)
t = +"abcde"
t[0] = "X"
p t
t[-1] = "Z"
p t
t[2] = "YY"
p t
# index == length appends (matches CRuby); out-of-range raises IndexError
# carrying the original index
g = +"abc"
g[3] = "d"
p g
oob = +"abc"
begin; oob[5] = "x"; rescue IndexError => e; puts "#{e.class}: #{e.message}"; end
begin; oob[-4] = "x"; rescue IndexError => e; puts "#{e.class}: #{e.message}"; end

# +@ / -@ as plain values
p(+"plus")
p(-"minus")

# concat / []= also work on a dup'd string
d = "orig".dup
d.concat("!")
d[0] = "O"
p d
