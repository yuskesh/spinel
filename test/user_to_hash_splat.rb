# `**obj` where obj defines #to_hash converts through to_hash, as CRuby does.
class Opts
  def to_hash = { a: 1, b: 2 }
end
def take(a:, b:) = [a, b]
p take(**Opts.new)

# into a keyword-rest callee
def collect(**o) = o
p collect(**Opts.new)

# an explicit keyword alongside the converted splat
p collect(x: 9, **Opts.new)

# a converter held in a local
o = Opts.new
p take(**o)
