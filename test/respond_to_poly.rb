# respond_to?(:m) on a poly receiver must answer per runtime value, not from a
# union-wide static approximation.
#
# `value.respond_to?(:to_css) ? value.to_css : value` where value is a String
# from some call sites and a #to_css-defining user class from others: the
# analyze probe types `value.to_css` (Color defines it) and reported a static
# true, so the String took the wrong branch and returned "" instead of itself.
#
# Fix: for a poly receiver whose method is defined by a user class, emit a
# runtime cls_id check (like the poly is_a? path) -- a builtin value, which
# cannot define a user protocol method, answers false.

class Color
  def initialize(s) = (@s = s)
  def to_css = @s
end
class Named
  def initialize(n) = (@n = n)
  def to_css = "named-#{@n}"
end

def coerce(value) = value.respond_to?(:to_css) ? value.to_css : value

# 1. A String member takes the else branch (returns itself).
p coerce("#fef6e0")               #=> "#fef6e0"
# 2. A #to_css-defining member responds.
p coerce(Color.new("#abc"))       #=> "#abc"
# 3. A second class defining the method also responds.
p coerce(Named.new("red"))        #=> "named-red"

# 4. A method no class defines: false for every runtime value.
def has_bogus(v) = v.respond_to?(:nonexistent_xyz)
p has_bogus("s")                  #=> false
p has_bogus(Color.new("x"))       #=> false

# 5. A universal Object method: true for every value.
def has_to_s(v) = v.respond_to?(:to_s)
p has_to_s("s")                   #=> true
p has_to_s(Color.new("x"))        #=> true
p has_to_s(42)                    #=> true

# 6. Concrete (non-poly) receivers are unaffected.
p "hi".respond_to?(:upcase)       #=> true
p Color.new("z").respond_to?(:to_css)  #=> true
p 5.respond_to?(:to_css)          #=> false

puts "done"
