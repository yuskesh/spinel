# The value form of `a && b` / `a || b` widens both arms to the unified
# result type. When the left is a nullable container pointer (hash or
# array) and the result is poly, the kept-left arm must box the temp;
# it previously emitted the raw pointer, producing a C ternary with
# mismatched operand types (sp_RbVal vs sp_PolyPolyHash *). A NULL
# pointer boxes to nil so the widened value stays falsy. Shape from the
# Doom gem's player_physics.rb: `picked && picked[idx]` where
# `picked = @item_pickup&.picked_up` is a Hash or nil.

class Pickup
  attr_reader :picked_up

  def initialize
    @picked_up = {}
  end

  def mark(idx)
    @picked_up[idx] = true
  end
end

class Game
  def initialize(pickup)
    @item_pickup = pickup
  end

  def check(idx)
    picked = @item_pickup&.picked_up
    if picked && picked[idx]
      puts "picked #{idx}"
    else
      puts "free #{idx}"
    end
  end

  def value_of(idx)
    picked = @item_pickup&.picked_up
    picked && picked[idx]
  end
end

pickup = Pickup.new
pickup.mark(1)
Game.new(pickup).check(1)
Game.new(pickup).check(2)
Game.new(nil).check(1)
p Game.new(pickup).value_of(1)
p Game.new(nil).value_of(1)

# Same shape over a nullable poly array left.
class Holder
  attr_reader :items

  def initialize
    @items = []
  end

  def add(v)
    @items << v
  end
end

class User
  def initialize(holder)
    @holder = holder
  end

  def peek(idx)
    items = @holder&.items
    v = items && items[idx]
    if v
      puts "got #{v}"
    else
      puts "none"
    end
  end
end

h = Holder.new
h.add("a")
h.add(7)
User.new(h).peek(0)
User.new(h).peek(1)
User.new(h).peek(5)
User.new(nil).peek(0)

# `||` keeps a truthy container left in a poly result.
class Fallback
  def initialize(holder)
    @holder = holder
  end

  def items_or_flag
    items = @holder&.items
    items || :missing
  end
end

p Fallback.new(h).items_or_flag
p Fallback.new(nil).items_or_flag
