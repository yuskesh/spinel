# Multiple assignment edges: an explicit-nil RHS element, and splat-underflow
# where post-splat targets run past the available elements (they land nil).
# Distinct variable names per statement avoid cross-statement type unification.

# explicit nil element (was a `void` temp build error)
n1a, n1c = [1, nil]
p [n1a, n1c]
n2x, n2y = [nil, nil]
p [n2x, n2y]
n3a, n3b, n3c = [1, nil, 3]
p [n3a, n3b, n3c]

# a non-literal nil element still runs for its side effects
def sideff_nil
  puts "side-effect ran"
  nil
end
v1a, v1b = [1, sideff_nil]
p [v1a, v1b]                      # side-effect ran, then [1, nil]

# splat-underflow: post-splat targets nil-fill left-to-right past the splat
u1a, *u1b, u1c = [1]
p [u1a, u1b, u1c]                 # [1, [], nil]
u2a, *u2b, u2c, u2d = [1, 2]
p [u2a, u2b, u2c, u2d]            # [1, [], 2, nil]
u3a, u3b, *u3c, u3d = [1, 2]
p [u3a, u3b, u3c, u3d]            # [1, 2, [], nil]

# non-underflow splat still correct
s1a, *s1b, s1c = [1, 2, 3, 4]
p [s1a, s1b, s1c]                 # [1, [2, 3], 4]
s2a, *s2b, s2c, s2d = [1, 2, 3, 4, 5]
p [s2a, s2b, s2c, s2d]           # [1, [2, 3], 4, 5]
s3a, s3b, *s3c, s3d = [1, 2, 3, 4, 5]
p [s3a, s3b, s3c, s3d]           # [1, 2, [3, 4], 5]
