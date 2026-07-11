x = 42
y = -7
name = "matz"
p "#{x}-#{y}"
p "hello #{name}!"
p "#{x}"
p "a" "b"
f = 3.14
p "f=#{f}"
b = true
p "b=#{b} nb=#{false}"
s = nil
p "nil=[#{s}]"
sym = :ok
p "sym=#{sym}"
arr = [1, "two"]
p "arr=#{arr}"
big = -9223372036854775807
p "big=#{big}"
p "multi #{x} and #{name} and #{f} end"
p "tab\t nl\n q\" bs\\ pct% #{x}"
h = { a: 1 }
p "h=#{h}"
p "#{ z = x + 1; z }"
vals = [0.0, -0.0, 1.0, -1.0, 2.5, -2.5, 0.1, 1.0/3.0, 999999999999999.0,
        1e15, -1e15, 1e16, 123456789.0, 3.14159, 1e-5, 0.0001, 1.5e20,
        -999999999999999.0, 42.0, 100.0]
vals.each { |v| puts v.to_s; puts "i=#{v}" }
puts (0.1 + 0.2).to_s
