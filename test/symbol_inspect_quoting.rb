# A symbol prints unquoted when its name is a plain identifier (incl. a
# constant), an @ivar/@@cvar/$gvar, a name ending in ? ! =, or a known operator
# method name; otherwise it is quoted like a string. Hash symbol keys use the
# short `name:` form, quoting the name when needed.

# plain identifiers and constants
p :abc
p :Foo
p :_x9

# variable-sigil names
p :@iv
p :@@cv
p :$g

# method-name suffixes
p :foo?
p :bar!
p :baz=

# every operator method symbol prints unquoted
p :+
p :-
p :*
p :/
p :%
p :**
p :==
p :===
p :!=
p :=~
p :!~
p :<
p :<=
p :>
p :>=
p :<=>
p :<<
p :>>
p :&
p :|
p :^
p :~
p :!
p :+@
p :-@
p :[]
p :[]=
p :`

# names that REQUIRE quoting
p :"a b"
p :"1x"
p :""
p :"has\"quote"
p :"with\nnewline"
p :"foo?bar"
p :"@"

# the .inspect method (string-returning path) agrees with p
puts :"a b".inspect
puts :plain.inspect
sym = :"x y"
puts sym.inspect

# symbol array and symbol-keyed hash
p [:"a b", :ok, :+, :"with space"]
p({a: 1, "k space": 2, normal: 3, "1bad": 4})
