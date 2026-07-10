# MatchData#values_at(:name) resolves each named group against this MatchData's
# own regexp group table, like MatchData#[]. Spinel routed a Symbol/String
# argument through the index accessor, which consulted a single first-seen global
# name table -- so a second named-capture regexp's names resolved to nil.
def one(s); s.match(/(?<a>\w)(?<b>\w)/).values_at(:a, :b); end
def two(s); s.match(/(?<c>\w)(?<d>\w)/).values_at(:c, :d); end
p one("hi")                    # ["h", "i"]
p two("xy")                    # ["x", "y"]

# String names and integer indices work too, including a mix.
def mix(s); s.match(/(?<x>\w)(?<y>\w)/).values_at("x", 2, 0); end
p mix("ab")                    # ["a", "b", "ab"]

# A name that did not participate yields nil.
def opt(s); s.match(/(?<p>\d)?(?<q>\w)/).values_at(:p, :q); end
p opt("z")                     # [nil, "z"]

# A poly-typed key (Symbol|Integer union) dispatches at runtime: a Symbol resolves
# by name, an Integer by index -- for both #values_at and #[].
def poly_va(s, byname); k = byname ? :y : 1; s.match(/(?<x>\w)(?<y>\w)/).values_at(k, 0); end
p poly_va("ab", true)          # ["b", "ab"]
p poly_va("ab", false)         # ["a", "ab"]
def poly_idx(s, byname); k = byname ? :x : 2; s.match(/(?<x>\w)(?<y>\w)/)[k]; end
p poly_idx("ab", true)         # "a"
p poly_idx("ab", false)        # "b"
