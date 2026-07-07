# .class yields a first-class (name-backed) Class value for every receiver
# kind: it compares with ===/== semantics, stringifies via to_s, and a Class
# scrutinee in when matches only Class/Module.
p 1.class
p "s".class
p :sym.class
p nil.class
p [1].class
p({}.class)
p 1.class == Integer
p "s".class.to_s
r = case :symbol.class
    when Symbol then "bar"
    when String then "bar"
    else "foo"
    end
p r
x = [1, "a"]
p x[0].class
puts "cls: #{1.class}"
