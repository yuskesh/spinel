# `Mod::X = v` assigns through a constant path. Spinel requires the constant to
# be declared first (registered by analysis); reassigning it then works, as the
# `M::X ||= v` / `M::X += v` forms already did. (Output is compared on stdout;
# Ruby's "already initialized constant" warning goes to stderr.)

module M
  COUNT = 0
  NAME = ""
end

M::COUNT = 5
p M::COUNT                  # 5

M::NAME = "spinel"
p M::NAME.upcase            # "SPINEL"

# reassign using the constant's own value
M::COUNT = M::COUNT + 10
p M::COUNT                  # 15

# nested constant path
module A
  module B
    LEVEL = 1
  end
end
A::B::LEVEL = 42
p A::B::LEVEL              # 42

# the operator / or-write forms still work alongside plain write
module N
  V = 1
end
N::V = 3
N::V += 4
p N::V                     # 7
N::V ||= 99
p N::V                     # 7 (already truthy)

# an empty [] / {} reassignment keeps the constant's specialized array/hash type
module E
  NUMS = [1, 2, 3]
  TBL = { "a" => 1 }
end
E::NUMS = []
p E::NUMS                  # []
E::TBL = {}
p E::TBL                   # {}
