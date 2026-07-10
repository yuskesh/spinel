# format/sprintf: dynamic width/precision (%*d, %.*f) consume the next
# positional argument; %<name>conv and %{name} reference a hash argument by
# key (missing keys raise KeyError). Unknown specs used to echo verbatim.
puts format("%*d", 5, 3)
puts format("%-*d|", 5, 3)
puts format("%*d", -5, 3) + "|"
puts format("%.*f", 2, 3.14159)
puts format("%08.*f", 3, 3.14159)
puts format("%<val>d", val: 42)
puts format("%<val>05d", val: 42)
puts format("%{val}", val: 42)
puts format("%{a}-%{b}", a: "x", b: 9)
puts sprintf("%<f>.2f", f: 1.2345)
begin
  format("%<nope>d", val: 1)
rescue KeyError => e
  puts "KeyError"
end
