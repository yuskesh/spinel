# A user object used as a pattern-match scrutinee must be asked to deconstruct
# itself: #deconstruct for an array pattern, #deconstruct_keys for a hash
# pattern -- including value-type objects, which the lowering previously
# excluded (they fell through to the reject path and raised "no pattern
# matched"). A value-type object is passed to the deconstruct method by value;
# a heap object by pointer.

class Pt
  def initialize(x, y) = (@x, @y = x, y)
  def deconstruct = [@x, @y]
  def deconstruct_keys(k) = { x: @x, y: @y }
end

# 1. Array pattern via #deconstruct.
case Pt.new(3, 4)
in [a, b]; p [a, b]; end                 #=> [3, 4]

# 2. Hash pattern via #deconstruct_keys.
case Pt.new(1, 2)
in { x:, y: }; p [x, y]; end             #=> [1, 2]

# 3. Hash pattern with a subset of keys.
case Pt.new(5, 6)
in { x: }; p x; end                      #=> 5

# 4. Array pattern with a guard.
case Pt.new(7, 8)
in [a, b] if a < b; p "ordered"; end     #=> "ordered"

# 5. A literal element is value-checked against the deconstructed array.
case Pt.new(9, 9)
in [a, 9]; p "y=#{a}"; end               #=> "y=9"

# 6. Arity drives which arm matches.
r = case Pt.new(1, 2)
    in [a, b, c]; "three"
    in [a, b]; "two"
    end
p r                                      #=> "two"

# 7. An object without #deconstruct falls through (no crash).
class Plain
  def initialize = (@v = 1)
end
res = case Plain.new
      in [a]; "arr"
      else; "else"
      end
p res                                    #=> "else"

# 8. A heap object (array ivar) with #deconstruct + a splat pattern.
class Box
  def initialize(items) = (@items = items)
  def deconstruct = @items
end
case Box.new([10, 20, 30])
in [x, *rest]; p [x, rest]; end          #=> [10, [20, 30]]

# 9. Struct inline synthesis (no user-defined #deconstruct): array and hash.
S = Struct.new(:x, :y)
case S.new(100, 200)
in [a, b]; p [a, b]; end                 #=> [100, 200]
case S.new(300, 400)
in { x:, y: }; p [x, y]; end             #=> [300, 400]

# 10. Data inline synthesis (a pure value-type): array and hash.
D = Data.define(:x, :y)
case D.new(5, 6)
in [a, b]; p [a, b]; end                 #=> [5, 6]
case D.new(7, 8)
in { x:, y: }; p [x, y]; end             #=> [7, 8]

puts "done"
