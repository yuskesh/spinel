# Array/Float/Integer conformance (KieranP #2295-2302)
p([1.1, 1.1, 2.2].uniq)          # #2295 FloatArray#uniq
p Array.new                       # #2296 bare Array.new as direct p arg
a = []
p a.sum(0.0)                      # #2297 empty array sum with Float init
p([1, 2, 3].sum(0.5))             # poly-array Float-init sum
p([2, 2].all?(2))                 # #2298 all? with a value pattern
p([1, 2, 3].none?(4..9))          # #2299 none? with a Range pattern
p([1, 5, 3].any?(4..9))           # any? Range pattern
p(["ab", "cd"].none?(/a/))        # #2300 none? with a Regexp pattern
p(["ab", "cd"].any?(/a/))         # any? Regexp pattern
p((-0.0).abs)                     # #2301 Float#abs of negative zero
p((0.0 * -1.0).magnitude)         # Float#magnitude negative zero
p(5.downto(1.5).to_a)             # #2302 downto with a Float limit
p(1.upto(3.5).to_a)               # upto with a Float limit
