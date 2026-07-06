# Nested proc literals: the emitter must not splice the inner _proc_ function
# into the middle of the outer one (both built into the same buffer before).
f = -> { -> { 41 }.call + 1 }
puts f.call
g = proc { -> { "in" }.call + "ner" }
puts g.call
