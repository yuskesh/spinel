# Hash variants (SymPolyHash, StrPolyHash, PolyPolyHash, ...) have different
# struct layouts. Type inference must not let a local and the ivar backing it
# settle on DIFFERENT variants, or codegen casts the pointer across layouts
# and reads garbage / corrupts the heap.

# Shape (a): `opts = @menu.options`. The ivar behind the reader is a
# SymPolyHash, but a second (dead-branch) assignment with string keys widens
# the LOCAL to a different variant. Reads through the local then walk the
# wrong struct layout.
class Menu
  attr_reader :options
  def initialize
    @options = {
      new_game: ["Start new game", 1],
      quit:     ["Quit to DOS", 9]
    }
  end
end

class Game
  def initialize
    @menu = Menu.new
  end

  def run
    opts = @menu.options
    if @menu.nil?
      opts = { "fallback" => ["none", 0] }
    end
    puts opts[:new_game][0]
    puts opts[:quit][1]
    opts[:load_game] = ["Load saved game", 3]
    puts opts[:load_game][0]
    puts opts.size
  end
end

Game.new.run

# Shape (b): `@cache ||= {}` where other writes (poly keys) make @cache a
# PolyPolyHash, but the empty literal keeps its default variant. The literal's
# constructor then disagrees with the ivar's declared C type.
class LineCache
  def store(key, val)
    @cache ||= {}
    @cache[key] = val
  end

  def load(key)
    @cache ||= {}
    @cache[key]
  end

  def count
    @cache ||= {}
    @cache.size
  end
end

lc = LineCache.new
lc.store(:alpha, [10, 20])
lc.store("beta", [30, 40])
lc.store(7, [50, 60])
puts lc.load(:alpha).inspect
puts lc.load("beta").inspect
puts lc.load(7).inspect
puts lc.count
