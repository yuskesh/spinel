# Array#min / #max / #minmax (no block) on String arrays, by String#<=> (byte
# comparison). Previously rejected for String arrays while Integer/Float arrays
# worked. Receivers route through a per-kind identity method so the typed
# runtime path is exercised rather than a compile-time fold.
def sw(x); x; end
def si(x); x; end
def sf(x); x; end

p sw(%w[banana apple cherry]).min
p sw(%w[banana apple cherry]).max
p sw(%w[banana apple cherry]).minmax
# byte ordering: uppercase precedes lowercase; lexicographic, not numeric
p sw(%w[Z a]).min
p sw(%w[10 9 100]).max
p sw(%w[b a a c]).min
# single element
p sw(["only"]).min
p sw(["only"]).minmax

# empty -> nil
def empty; a = ["x"]; a.clear; a; end
p empty.min
p empty.max

# Integer / Float arrays remain unaffected
p si([3, 1, 2]).min
p si([3, 1, 2]).max
p si([3, 1, 2]).minmax
p sf([3.0, 1.0, 2.0]).min
p sf([3.0, 1.0, 2.0]).minmax
