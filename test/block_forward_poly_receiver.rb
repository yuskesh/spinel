# A &block-forwarding method reached through an Array element (a typed array
# element = concrete class; a heterogeneous array element = poly value): the
# block must land, not drop to a C compile error or a silent zero iterations.
class Roster
  def initialize; @items = {}; end
  def add(key, value); @items[key] = value; end
  def each_item(&block); @items.each_value(&block); end
end

# typed array element -> concrete Roster receiver
wrapped = [Roster.new]
wrapped[0].add(1, "x")
count = 0
wrapped[0].each_item { |v| count += 1 }
puts "count=#{count}"

# heterogeneous array element -> poly receiver
def make_pair
  r = Roster.new
  r.add(1, "x")
  [r, { 1 => false }]
end
pair = make_pair
roster = pair[0]
c2 = 0
roster.each_item { |v| c2 += 1 }
puts "c2=#{c2}"
