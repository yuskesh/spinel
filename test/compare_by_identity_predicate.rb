# Hash#compare_by_identity? reports false for an ordinary (value-keyed) hash.
# The mutating Hash#compare_by_identity is rejected at compile time, since
# spinel compares hash keys by value and cannot switch a hash to identity
# comparison; emitting it as a no-op would silently diverge from CRuby. This
# test pins the predicate's correct value-keyed answer.
def s(x); x; end
p s({}).compare_by_identity?
p s({ "a" => 1 }).compare_by_identity?
h = {}
p h.compare_by_identity?
