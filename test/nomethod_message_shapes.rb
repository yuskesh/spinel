# The NoMethodError a gated (unresolved) call raises names the receiver the
# way CRuby does: nil/true/false read as themselves, everything else as
# "an instance of <Class>" from the runtime value -- not the lattice name
# ("for poly").
class Cell
  def initialize(x)
    @x = x
  end
end

begin
  [Cell.new(1), Cell.new(2)].map(&:id)
rescue NoMethodError => e
  puts e.message
end

begin
  [nil].each { |v| v.bogus }
rescue NoMethodError => e
  puts e.message
end

begin
  [true].each { |v| v.bogus }
rescue NoMethodError => e
  puts e.message
end

begin
  [3.5].each { |v| v.bogus }
rescue NoMethodError => e
  puts e.message
end
