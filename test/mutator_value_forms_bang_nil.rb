# String mutators as expressions (with lvalue write-back and the
# nil-when-unchanged bang contract), Array#insert with several values
# (negative index normalized once), Array#flatten!/uniq!/compact! as
# expressions returning self-or-nil.
p "ab".concat("cd")
s = "ab"
p s.concat("cd", "ef")
p s
p "ab" << "x"
t = "world"
p t.prepend("hello ", "big ")
p t
u = "hello"
p u.insert(2, "XX")
p u
p "hey".insert(-1, "!")
v = "abc"
p v.replace("zzz")
p v
w = "Hello World"
p w.gsub!(/o/, "0")
p w
p w.gsub!(/zzz/, "x")
p "abc".upcase!
p "ABC".upcase!
x = "  hi  "
p x.strip!
p x
p "abc".reverse!
y = "hello"
p y.slice!(1..2)
p y
p "hello".slice!(0)
p "hi".slice!(99)
p [1, 2, 3].insert(1, 9, 8)
p [1, 2, 3].insert(-2, 9, 8)
p ["a", "c"].insert(1, "b", "x")
p [1, :a].insert(1, 9, 8)
p [1, [2, [3]]].flatten!
p [1, 2].flatten!
p [1, 2, 3].uniq!
p [1, 1, 2].uniq!
p ["a", "a"].uniq!
p [1, nil, 2].compact!
p [1, 2].compact!
