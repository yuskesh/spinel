# A custom initialize on a Data/Struct value class delegates to super to set
# the members. Data passes its members as keywords (super(x: ..., y: ...)),
# which must map each member by name -- not positionally, which would drop the
# whole keyword hash into the first member's slot. Args are routed through a
# method so the values are not constant-folded at the call site.

Point = Data.define(:x, :y) do
  def initialize(x:, y:)
    super(x: x * 100, y: y + 1)
  end
end

def make_point(a, b)
  Point.new(x: a, y: b)
end

p make_point(5, 2)
p make_point(3, 7)

# Struct with a positional super still assigns members positionally.
Tally = Struct.new(:a, :b) do
  def initialize(a, b)
    super(a * 2, b * 3)
  end
end

def make_tally(a, b)
  Tally.new(a, b)
end

t = make_tally(4, 5)
p [t.a, t.b]

# A keyword super that omits a member raises ArgumentError, as in CRuby.
Pair = Data.define(:m, :n) do
  def initialize(m:, n:)
    super(m: m)
  end
end

begin
  Pair.new(m: 1, n: 2)
rescue ArgumentError => e
  puts "error: #{e.message}"
end
