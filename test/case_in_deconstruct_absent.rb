# An array pattern against an object with no #deconstruct never matches; the
# arm fails closed and the else branch is taken (as in CRuby).
class NoDe; end
r = case NoDe.new
    in [a, b] then "matched"
    else "nomatch"
    end
p r
