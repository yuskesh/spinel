# Reading an instance variable that is never assigned anywhere in the program
# is nil in Ruby, not a compile error. A top-level method reading such an ivar
# gets a nil-valued slot in the Toplevel pseudo-class.
def f; @x; end
p f            # nil

p @top        # nil, read at top level

def g; @z.nil?; end
p g           # true

def h; @w.inspect; end
p h           # "nil"

# defined? still reports it as absent until assigned.
p defined?(@never)   # nil
