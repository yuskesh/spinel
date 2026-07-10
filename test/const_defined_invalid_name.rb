# Module#const_defined? validates the constant name. A defined name answers
# true, an undefined-but-well-formed name answers false, and a malformed name
# (lowercase / leading underscore / a non-identifier byte) raises NameError
# "wrong constant name <name>" -- Spinel previously answered false silently.
# (Only a literal name argument is folded; a runtime name rejects separately.)
module M
  X = 1
end

p M.const_defined?("X")   # true
p M.const_defined?(:X)    # true
p M.const_defined?("Y")   # false (well-formed, undefined)
p M.const_defined?(:Zz)   # false
p M.const_defined?("X1")  # false

def check(&blk)
  blk.call
  puts "no raise"
rescue NameError => e
  puts e.message
end

check { M.const_defined?("name") }
check { M.const_defined?("_Foo") }
check { M.const_defined?("@x") }
check { M.const_defined?("1A") }
check { M.const_defined?("A B") }
check { M.const_defined?("") }
check { M.const_defined?(:lower) }
