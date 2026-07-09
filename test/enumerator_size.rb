# Enumerator#size: the size protocol that never iterates. A materialized
# enumerator (a blockless builtin #each / each_slice / each_cons) reports its
# snapshot length; a fiber-backed generator (Enumerator.new) reports the size it
# was given -- nil when none, a stored value (Integer / Float::INFINITY), or the
# result of a stored callable (Proc/lambda), which #size calls lazily.

# --- a generator with no size -> nil ---
p Enumerator.new { |y| y << 1 }.size

# --- native materialized enumerators report their length ---
p [10, 20, 30].each.size
p [1, 2, 3, 4].each_slice(2).size
p [1, 2, 3, 4].each_cons(2).size

# --- Enumerator.new(size) { |y| }: value / infinity ---
p Enumerator.new(5) { |y| y << 1 }.size
p Enumerator.new(Float::INFINITY) { |y| y << 1 }.size

# --- a stored callable size, called lazily by #size ---
p Enumerator.new(lambda { 42 }) { |y| y << 1 }.size
p Enumerator.new(proc { 7 }) { |y| y << 1 }.size

# a size callable closing over a local
n = 9
p Enumerator.new(-> { n * 2 }) { |y| y << 1 }.size

# --- #size never iterates the body ---
e = Enumerator.new(3) { |y| raise "size must not iterate"; y << 1 }
p e.size

# --- a generator with an explicit size still iterates normally ---
g = Enumerator.new(2) { |y| y << :a; y << :b }
p g.size
p g.to_a
p g.size
