# Integer/String #eql? and #equal? on typed receivers.
# eql?/equal? are value-equality with NO numeric coercion, unlike ==.

a = 1
b = 1
c = 2
f = 1.0
s = "x"
t = "x"
u = "y"

# Integer#eql? : true only for an Integer-typed, value-equal arg
puts a.eql?(b)      # true
puts a.eql?(c)      # false
puts a.eql?(f)      # false  (no coercion: 1 != 1.0)
puts 1.eql?(1)      # true   (bare literals)
puts 1.eql?(1.0)    # false

# Integer#equal? : value identity for fixnums (same as eql? here)
puts a.equal?(b)    # true
puts a.equal?(c)    # false
puts 5.equal?(5)    # true

# String#eql? : byte-equal only for a String-typed arg
puts s.eql?(t)      # true
puts s.eql?(u)      # false
puts "x".eql?("x")  # true

# String#equal? : object identity is POINTER identity. Each literal
# OCCURRENCE is its own static object (no cross-occurrence merging), so two
# textually equal literals are distinct objects, matching plain CRuby.
# Aliasing (t = s) still answers truthfully. The one residue: re-evaluating
# the SAME occurrence (a literal in a loop) yields one object -- the
# frozen-string-literal semantics spinel's immutable strings always had.
puts s.equal?(s)    # true   (same variable)
puts s.equal?(t)    # false  (two occurrences: distinct objects)
puts s.equal?(u)    # false  (different value)
puts "x".equal?("x") # false (two occurrences: distinct objects)

# typed receiver, polymorphic arg (element drawn from a mixed array)
mix = [1, "x", 1.0]
puts a.eql?(mix[0]) # true
puts a.eql?(mix[1]) # false  (arg is a String)
puts s.eql?(mix[1]) # true
puts s.eql?(mix[0]) # false  (arg is an Integer)
