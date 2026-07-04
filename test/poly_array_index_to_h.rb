class Sector
  attr_reader :name

  def initialize(name)
    @name = name
  end
end

class Flat
  attr_reader :name

  def initialize(name)
    @name = name
  end
end

# --- index(value) on a poly array of objects (Doom: @map.sectors.index(sector)) ---
a = Sector.new("a")
b = Sector.new("b")
c = Sector.new("c")
sectors = [a, b, c]
puts sectors.index(b)
puts sectors.index(a)
puts sectors.index(Sector.new("z")).inspect

# --- index(value) on a mixed poly array of strings and ints ---
mixed = [1, "two", 3, "four"]
puts mixed.index("two")
puts mixed.index(3)
puts mixed.index("missing").inspect

# --- index { |x| cond } on a poly array ---
puts sectors.index { |s| s.name == "c" }
puts sectors.index { |s| s.name == "nope" }.inspect
puts mixed.index { |x| x == "four" }

# --- to_h { |f| [f.name, f] } producing string keys (Doom: flats.to_h) ---
flats = [Flat.new("floor"), Flat.new("ceil"), Flat.new("sky")]
by_name = flats.to_h { |f| [f.name, f] }
puts by_name.size
puts by_name["floor"].name
puts by_name["ceil"].name
puts by_name["sky"].name

# --- to_h with symbol keys over a poly array ---
things = [1, "two"]
sym_h = things.to_h { |t| [t.class.to_s.downcase.to_sym, t.to_s] }
puts sym_h.size
puts sym_h[:integer]
puts sym_h[:string]
