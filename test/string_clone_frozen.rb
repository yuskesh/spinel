# String#clone preserves the frozen state; String#dup always returns unfrozen.
def id(x) = x

# clone keeps frozen, dup clears it
p id("x").freeze.clone.frozen?     # true
p id("x").freeze.dup.frozen?       # false
# unfrozen original: both stay unfrozen
p id("abc").clone.frozen?          # false
p id("abc").dup.frozen?            # false
# value is preserved across clone
p id("hi").freeze.clone            # "hi"
# through a local variable
s = "y".freeze
c = s.clone
p c.frozen?                        # true
p c                                # "y"
d = s.dup
p d.frozen?                        # false
# a frozen clone still raises on mutation
begin
  c << "z"
rescue FrozenError => e
  p e.class                        # FrozenError
end
# literal receiver
p "lit".freeze.clone.frozen?       # true
