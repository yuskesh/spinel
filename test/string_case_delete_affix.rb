# String#swapcase, #delete_prefix, and #delete_suffix return new strings. Each
# has a runtime helper already; this exercises the typed-String dispatch arms and
# the inferred String return type (so the results chain and print). Receivers go
# through a method param to defeat constant-folding.
def ss(x); x; end

# swapcase flips the case of each letter, leaving non-letters alone
p ss("Hello World").swapcase
p ss("aBcDeF 123").swapcase
p ss("").swapcase

# delete_prefix / delete_suffix remove a leading/trailing match, else unchanged
p ss("hello").delete_prefix("he")
p ss("hello").delete_prefix("xx")
p ss("hello").delete_suffix("lo")
p ss("hello").delete_suffix("xx")

# they return Strings, so results chain
p ss("hello").delete_prefix("he").delete_suffix("lo")
p ss("Hello").swapcase.delete_prefix("h")

# a non-literal argument works too
affix = ss("foo")
p ss("foobar").delete_prefix(affix)
