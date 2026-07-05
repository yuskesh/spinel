# A splat multi-assign with fewer RHS elements than the fixed targets empties
# the splat and nil-fills the trailing targets, rather than wrapping indices.
def s(x); x; end
a, *b, c = s([1]);          p [a, b, c]
a, *b, c = s([1, 2]);       p [a, b, c]
a, *b, c = s([1, 2, 3]);    p [a, b, c]
a, *b, c = s([1, 2, 3, 4]); p [a, b, c]
w, *x, y, z = s([1, 2]);    p [w, x, y, z]

# The same clamping applies when a post-splat target is an instance variable.
class C
  def set(arr)
    a, *b, @c = arr
    p [a, b, @c]
  end
end
C.new.set([1])
C.new.set([1, 2, 3])
