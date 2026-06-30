# `recv.send(name)` / `__send__` / `public_send` with a NON-literal name dispatches
# over the method names that appear as symbol/string literals in the program. The
# receiver's type bounds which names resolve; the argument count selects the arm.
# A name not in that set (or not a method on the receiver) raises NoMethodError.
def s(x); x; end

# symbol held in a local
a = s([3, 1, 2])
m = :sort
p a.send(m)              # [1, 2, 3]
m = :length
p a.send(m)             # 3
m = :first
p a.send(m)             # 3

# argument count selects the arm (push needs one arg; first takes none)
b = s([1, 2])
op = :push
b.send(op, 9)
p b                     # [1, 2, 9]

# string name held in a local (interns to the same method)
nm = "reverse"
p s([1, 2, 3]).send(nm)   # [3, 2, 1]

# dynamically chosen among a set, on a typed (literal) string receiver
%w[upcase reverse].each { |k| p "abc".send(k) }   # "ABC", "cba"

# __send__ and public_send aliases
p a.__send__(:max)      # 3
p a.public_send(:min)   # 1

# a name that is not a method on the receiver raises NoMethodError
bad = :no_such_method
begin
  a.send(bad)
rescue NoMethodError
  puts "NoMethodError"
end
