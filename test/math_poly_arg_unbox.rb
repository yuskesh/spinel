# Math.* with a poly-typed argument (a value that unifies to Integer|Float)
# must unbox at the call boundary via sp_poly_to_f. Previously only a plain
# TY_INT arg got a (double) cast; a poly arg passed straight through as an
# sp_RbVal into the mrb_float parameter -- invalid C. Shape from the Doom
# gem's distance helpers (player_physics.rb, sector_actions.rb):
#   Math.sqrt(dx * dx + dy * dy)  # dx, dy flow both Integer and Float

def dist(dx, dy)
  Math.sqrt(dx * dx + dy * dy)
end

puts dist(3, 4)
puts dist(1.5, 2.0)

def pick(int_side)
  int_side ? 2 : 2.0
end

x = pick(true)   # poly: Integer|Float
y = pick(false)

puts Math.sqrt(x).round(4)
puts Math.sin(y).round(4)
puts Math.log(x).round(4)
puts Math.log(8, x)
puts Math.log2(x)
puts Math.log10(y).round(4)
puts Math.atan2(x, y).round(4)
puts Math.hypot(x, y).round(4)
puts Math.ldexp(y, 3)
lg = Math.lgamma(x)
lg0 = lg[0]
puts lg0
