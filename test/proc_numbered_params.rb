# Numbered parameters (_1.._9) in first-class procs/lambdas: they surface as
# plain local reads with no parameters node, bind boxed off the argument
# side-channel, and count toward lambda arity.
f = -> { _1 * 10 }
p f.call(5)
g = proc { _1 + _2 }
p g.call(1, 2)
p [1, 2, 3].map { _1 * 2 }
h = -> { _2 }
p h.call(7, 8)
begin
  h.call(1)
rescue ArgumentError
  puts "arity"
end
