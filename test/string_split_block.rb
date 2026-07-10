# String#split with a block yields each substring (rather than ignoring the
# block and returning the array). Spinel previously never ran the block. The
# call is used in statement position, collecting via the block's side effects --
# matching how String#scan with a block already behaves.
def sp1(x); r = []; x.split(",") { |p| r << p }; r; end
p sp1("a,b,c")                 # ["a", "b", "c"]

# Whitespace mode (no separator / a single space).
def spws(x); r = []; x.split { |w| r << w.upcase }; r; end
p spws("  foo   bar ")         # ["FOO", "BAR"]

# Explicit separator with a limit.
def splim(x); r = []; x.split(",", 2) { |p| r << p }; r; end
p splim("a,b,c,d")             # ["a", "b,c,d"]

# The block runs for its side effects; the method returns another value.
def total(x); n = 0; x.split("-") { |p| n += p.length }; n; end
p total("aa-bbb-c")            # 6

# Each yielded piece is a String (supports String methods in the block).
def joined(x); acc = ""; x.split(":") { |p| acc += p.reverse }; acc; end
p joined("ab:cd")              # "badc"

# An empty result yields nothing.
def none(x); c = 0; x.split(",") { |p| c += 1 }; c; end
p none("")                     # 0
