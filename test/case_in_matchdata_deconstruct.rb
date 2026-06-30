# A hash pattern against a MatchData scrutinee matches via
# MatchData#deconstruct_keys: the regex's named captures become a symbol-keyed
# hash, so `case md in {name:}` binds the capture. Covers case/in (single,
# multiple, class check, capture binding, no-match -> else), the rightward
# `=>` form, and a nil (no-match) MatchData against `in nil`.

m = "2026-06".match(/(?<yr>\d+)-(?<mo>\d+)/)

case m
in { mo: } then p mo
end

case m
in { yr:, mo: } then p [yr, mo]
end

case m
in { mo: String => s } then p s
else p :no
end

case m
in { yr: bound } then p bound
end

case m
in { nope: } then p :yes
else p :fellthrough
end

# rightward assignment form
m => { yr:, mo: }
p [yr, mo]

# a no-match MatchData is nil; `in nil` matches it
miss = "abc".match(/(?<d>\d+)/)
case miss
in { d: } then p d
in nil then p :nilmatch
end
