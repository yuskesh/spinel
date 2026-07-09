# A bare `new(...)` (implicit self) inside a Data/Struct singleton factory must
# forward its arguments to the generated constructor -- positional AND keyword --
# exactly like an explicit `Klass.new(...)`.
#
# A prior bug: the self-call codegen path emitted the constructor only with args
# when a USER `initialize` existed. A Data/Struct class has no user initialize
# (its constructor is generated per-member), so `initm < 0` and every argument
# was dropped -- `def self.default = new(x: 1)` compiled to `sp_Klass_new()`,
# yielding a zero-arg call the C constructor rejects. The receiver path
# (`Klass.new(...)`) already did the member-wise fill; the self-call path now
# mirrors it.

Size = Data.define(:w, :h) do
  def self.square(n) = new(w: n, h: n)   # bare new, keyword args
  def self.unit = new(1, 1)              # bare new, positional args
end

sq = Size.square(4)
puts sq.w        # 4
puts sq.h        # 4

u = Size.unit
puts u.w         # 1
puts u.h         # 1

# Nested: a factory whose bare `new` takes another Data value as a field, and a
# default-carrying factory (the shell-model shape).
Box = Data.define(:name, :size, :open) do
  def self.default = new(name: "box", size: Size.square(3), open: false)
end

b = Box.default
puts b.name      # box
puts b.size.w    # 3
puts b.open      # false

b2 = b.with(open: true)
puts b2.open     # true
puts b2.name     # box
