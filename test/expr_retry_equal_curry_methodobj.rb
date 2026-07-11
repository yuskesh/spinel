# p(multi) / loop as expressions, retry with ensure, Array/Hash#equal?,
# curry[a, b] multi-arg application, and &method(:kernel_builtin) procs.
x = p(1, 2)
p x
y = p(5)
p y
z = loop { break }
p z
w = loop { break 42 }
p w
c = true
begin
  raise "x"
rescue
  (c = false; retry) if c
ensure
  puts "ens"
end
puts "done"
i = 0
begin
  i += 1
  raise "boom" if i < 3
  puts "body done #{i}"
rescue
  retry
ensure
  puts "ens #{i}"
end
r = begin
  raise "x"
rescue
  retry_done ||= 0
  retry_done += 1
  retry_done < 2 ? retry : "recovered"
ensure
  puts "ens2"
end
p r
a = [1, 2]
b = a
p a.equal?(b)
p a.equal?([1, 2])
s = "x"
p s.equal?(s)
h = { a: 1 }
p h.equal?(h)
add132 = proc { |a, b, c| a + b + c }
p(add132.curry[1, 2][3])
p(add132.curry[1][2][3])
p(add132.curry[1, 2, 3])
p([1, 2, 3].map(&method(:Integer)))
p(["1", "2"].map(&method(:Integer)))
p([1.7, 2.2].map(&method(:Integer)))
m = method(:puts)
m.call("hi")
def shout(s); puts s; end
m = method(:shout)
m.call("hi")
