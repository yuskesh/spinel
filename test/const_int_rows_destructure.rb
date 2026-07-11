# A poly-array CONSTANT whose rows are all int-array literals (a frozen
# direction table) destructures its 2+ block params as ints, so methods
# called with the destructured values keep typed int params -- one such
# call site must not poison every downstream caller (e.g. a hot
# "#{x}-#{y}" key builder) onto the boxed slow path.
DIRECTIONS = [
  [-1, -1], [-1, 0], [-1, 1],
  [0, -1], [0, 1],
  [1, -1], [1, 0], [1, 1],
].freeze

def make_key(x, y)
  "#{x}-#{y}"
end

def neighbours(cx, cy)
  DIRECTIONS.filter_map do |rel_x, rel_y|
    nx = cx + rel_x
    ny = cy + rel_y
    next if nx < 0 || ny < 0
    make_key(nx, ny)
  end
end

p neighbours(1, 1)
p neighbours(0, 0)
p make_key(7, 8)

# each with destructure over the same constant
sum = 0
DIRECTIONS.each { |dx, dy| sum += dx * 10 + dy }
p sum

# a non-uniform constant stays poly (no wrong narrowing)
MIXED = [[1, 2], "s"].freeze
kinds = MIXED.map { |m| m.class.to_s }
p kinds
