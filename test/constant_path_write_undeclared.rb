# Nested constant assignment to a constant that was NOT pre-declared inside the
# module body: `Mod::X = v` and the or/and-write forms. The leaf name is
# registered flat; an or/and-write-only constant defaults to nil so its `||=`
# assigns on first use. (Ruby's "already initialized constant" warnings go to
# stderr; only stdout is compared.)
module M; end

M::X = 5
p M::X                    # 5

M::S = "hi"
M::S = M::S + "!"         # reassignment reads the current value
p M::S                    # "hi!"

# ||= on a fresh constant assigns (undefined is nil/falsy); a second ||= no-ops.
M::B ||= 5
p M::B                    # 5
M::B ||= 9
p M::B                    # 5

# fresh ||= with a string value.
M::T ||= "x"
p M::T                    # "x"

# ||= on a constant given a definite value first keeps that value.
M::A = 3
M::A ||= 99
p M::A                    # 3

# &&= on a predefined constant reassigns.
M::C = 1
M::C &&= 2
p M::C                    # 2
