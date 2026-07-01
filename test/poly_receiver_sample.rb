# Array#sample on a poly receiver -- a value whose array-ness is only known at
# runtime, e.g. an object field or hash value that holds an array. It previously
# fell through the poly dispatch and returned nil; it must pick an element.
N = Struct.new(:node, :follow)
dic = { begin: N.new(:begin, []) }
node = dic[:begin]
node.follow << N.new(:a, [])
node.follow << N.new(:b, [])

s = node.follow.sample
p s.nil?            # false
p s.is_a?(N)        # true

# a plain poly array receiver
arr = [N.new(:x, []), N.new(:y, []), 100]
p arr.sample.nil?   # false

# empty poly value samples to nil
empty = N.new(:e, [])
p empty.follow.sample.nil?   # true
