# `if v.is_a?(K)` narrows a poly v to K's concrete type inside the then-branch,
# so a method only SOME union members define dispatches correctly instead of
# aborting "undefined method 'M' for poly".
#
# A poly parameter unioned across Array | String | Integer could not call
# `v.to_a` (Array-only) -- it dispatched over the whole union and aborted at
# runtime. Inside `if v.is_a?(Array)` the runtime type is proven, so reads of v
# narrow to a concrete type (unboxed at the read site, the machinery the nil
# guards already use). Array narrows to a poly array via a runtime normalizer,
# since a boxed array can be any element-typed representation.

# 1. Array-only method under an is_a?(Array) guard (gap 12).
def f(v)
  return v.to_a if v.is_a?(Array)
  v.to_s
end
p f([1, 2])                 #=> [1, 2]
p f("x")                    #=> "x"
p f(7)                      #=> "7"

# 2. is_a?/elsif chain, each arm a different type (gap 3 shape).
def g(v)
  if v.is_a?(String)
    v.upcase
  elsif v.is_a?(Array)
    v.map { |e| e * 2 }
  else
    v.to_s
  end
end
p g("hi")                   #=> "HI"
p g([1, 2])                 #=> [2, 4]
p g(9)                      #=> "9"

# 3. Recursive, multi-type return (the original gap 3 program).
def enc(v)
  if v.is_a?(String)
    out = "\""
    v.each_char { |c| out = out + c }
    out + "\""
  elsif v.is_a?(Array)
    "[" + v.map { |e| enc(e) }.join(",") + "]"
  else
    v.to_s
  end
end
p enc(["ab", "cd"])         #=> "[\"ab\",\"cd\"]"

# 4. Integer / Float narrowing to primitive ops.
def h(v)
  v.is_a?(Integer) ? v * 10 : v.to_s
end
p h(5)                      #=> 50
p h("q")                    #=> "q"
def flt(v)
  v.is_a?(Float) ? v + 1.0 : v.to_s
end
p flt(2.5)                  #=> 3.5
p flt("z")                  #=> "z"

# 5. Array narrowing works across element representations.
def sa(v)
  v.is_a?(Array) ? v.first : nil
end
p sa(["a", "b"])            #=> "a"
p sa([1.5, 2.5])            #=> 1.5

# 6. kind_of? is an alias.
def k(v)
  v.kind_of?(String) ? v.length : -1
end
p k("hello")                #=> 5
p k(42)                     #=> -1

# 7. Soundness: a write inside the branch disables narrowing (no miscompile).
def wr(v)
  if v.is_a?(Array)
    v = v.to_a
    v.length
  else
    0
  end
end
p wr([1, 2, 3])             #=> 3

puts "done"
