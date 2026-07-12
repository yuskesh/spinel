# Array#zip with a block: a SOLO param receives the boxed [e1, e2] tuple,
# two params auto-splat it, and the block form returns nil (value position
# included).
z = []
r = [1, 2, 3].zip([4, 5, 6]) { |t| z << t }
p r
p z
w = []
%w[a b].zip([1, 2]) { |pair| w << pair }
p w
q = []
[1, 2].zip([7, 8]) { |x, y| q << x + y }
p q
