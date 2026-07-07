# The statically-traceable send names must keep compiling and running after the
# genuinely-computed names are loud-rejected. A name is traceable when it is a
# literal, a variable assigned only literals, or a block parameter bound to
# program literals -- all of which the static arm set covers.

# a literal name
p "hi".send(:upcase)

# a variable assigned a symbol literal
m = :upcase
p "hi".send(m)

# a variable reassigned among symbol literals
sel = :reverse
p [1, 2, 3].send(sel)
sel = :first
p [1, 2, 3].send(sel)

# a block parameter bound to string literals
%w[upcase reverse].each { |k| p "abc".send(k) }

# __send__ / public_send aliases with a traceable name
op = :max
p [3, 1, 2].__send__(op)
q = :min
p [3, 1, 2].public_send(q)

# a traceable name that isn't a method on the receiver still raises NoMethodError
bad = :no_such_method
begin
  "hi".send(bad)
rescue NoMethodError
  puts "NoMethodError"
end
