class Item
  def initialize(tag); @tag = tag; end
  attr_reader :tag
end
module S
  def self.root; Item.new("R"); end
  def self.list
    [Item.new("a"), Item.new("b"), Item.new("c"),
     Item.new("d"), Item.new("e"), Item.new("f"),
     Item.new("g"), Item.new("h"), Item.new("i")]
  end
end

def f(table)
  i = 0
  while i < table.length
    table[i]
    i += 1
  end
end

bad = 0
i = 0
while i < 50
  combined = [S.root] + S.list
  bad += 1 if combined.length != 10
  f(combined)
  i += 1
end
puts bad
puts "done"
