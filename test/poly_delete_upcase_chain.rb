# String#delete then #upcase on a value that widened to poly, in a program
# where a user class (the bundled Set) also defines `delete`: the poly delete
# must keep the string possibility (unify with the class dispatch) instead of
# binding exclusively to the user class, and the boxed string transforms must
# stay poly-typed. Doom's texture parser hits this chain on WAD name fields:
# `data[offset, 8].delete("\x00").upcase`.
require 'set'

def maybe(s)
  return nil if s.empty?
  s
end

s = Set.new([1, :two])
s.delete(1)
p s.size

data = maybe("ab\x00\x00cd\x00\x00")
name = data[0, 4].delete("\x00").upcase
puts name
puts data[4, 4].delete("\x00").upcase

# delete through a poly slot that actually holds the Set.
mix = [s, "str"]
ps = mix[0]
ps.delete(2)
p s.size

# ... and one that holds a string.
puts mix[1].delete("t")

# The other boxed string transforms on a poly value.
y = maybe(" Hi ")
puts y.strip
puts y.reverse.strip
p maybe("word").upcase
