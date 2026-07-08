require "digest"

# Two hexdigest results held at once must not alias: the runtime helper
# returns a static buffer, and codegen dups a :cstring return onto the GC
# heap, so `a` keeps its value across the second call.
a = Digest::SHA256.hexdigest("x3")
b = Digest::SHA256.hexdigest("x9")
puts a
puts b
puts a == b

c = Digest::SHA1.hexdigest("x3")
d = Digest::SHA1.hexdigest("x9")
puts c
puts d
puts c == d
