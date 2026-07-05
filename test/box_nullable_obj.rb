# A method or hash lookup that can `return nil` yields a genuinely NULL object
# pointer. Boxing it into a poly value must produce nil, not a "truthy" object
# wrapping a NULL pointer -- otherwise `unless x` / `if x` passes and the first
# field/method read segfaults (doom's TextureManager#[] -> build_composite ->
# texture.width).
Pt = Struct.new(:x, :y)
class Cache
  def initialize; @h = {}; end
  def fetch(k); @h[k] ||= make(k); end
  def make(k)
    return nil if k < 0
    Pt.new(k, k * 2)
  end
end
c = Cache.new
a = c.fetch(3)
puts "a: #{a.x},#{a.y}"
b = c.fetch(-1)
puts "b nil? #{b.nil?}"
puts "b.x=#{b.x}" if b   # must NOT run (b is nil); without the fix b is a truthy NULL -> segv
puts "done"
