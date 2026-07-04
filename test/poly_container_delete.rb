require 'set'

# Array#delete on a poly (mixed-element) array: returns the deleted value,
# or nil when the value is absent, and shrinks the array in place.
class Marker
  def initialize(tag)
    @tag = tag
  end
end

m = Marker.new(1)
arr = [1, "two", :three, m]
p arr.delete("two")
p arr.length
p arr.delete(1)
v = arr.delete(m)
p v.nil?
p arr.length
p arr.delete(99)
p arr.length

# Hash#delete on a sym-keyed poly-valued hash: returns the value or nil.
h = { alpha: 30, beta: "door", gamma: 2.5 }
p h.delete(:beta)
p h.key?(:beta)
p h.delete(:missing)
p h.length

# Hash#delete on a poly-keyed poly-valued hash.
ph = { 1 => "one", "two" => 2, :three => 30 }
p ph.delete("two")
p ph.key?("two")
p ph.delete(9)
p ph.length

# Set#delete through the bundled stdlib package (array-backed).
s = Set.new([1, "a", :b])
s.delete("a")
p s.include?("a")
p s.size
s.delete(99)
p s.size
