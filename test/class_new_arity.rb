# Class#new must enforce initialize's arity: extra (or missing) positional args
# raise ArgumentError at the .new site, as in MRI. Previously the surplus args
# were silently dropped and construction succeeded.
class C; end
def f; C.new(7); end
begin
  f
  puts "no raise"
rescue ArgumentError => e
  puts e.message                 # wrong number of arguments (given 1, expected 0)
end

class D
  def initialize(a, b); @a = a; @b = b; end
end
def too_few; D.new(1); end
begin
  too_few
rescue ArgumentError => e
  puts e.message                 # given 1, expected 2
end

def too_many; D.new(1, 2, 3); end
begin
  too_many
rescue ArgumentError => e
  puts e.message                 # given 3, expected 2
end

# Exactly-right construction still works.
def ok; D.new(4, 5); end
p ok.class                       # D

# An initialize with an optional param accepts a range of counts.
class E
  def initialize(a, b = 10); @a = a; @b = b; end
end
def e1; E.new(1); end
def e2; E.new(1, 2); end
p e1.class                       # E
p e2.class                       # E
def e_bad; E.new(1, 2, 3); end
begin
  e_bad
rescue ArgumentError => e
  puts e.message                 # given 3, expected 1..2
end

# A splat initialize takes any count -- no arity error.
class F
  def initialize(*a); @a = a; end
end
def f_any; F.new(1, 2, 3, 4); end
p f_any.class                    # F
