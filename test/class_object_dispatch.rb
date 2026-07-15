# A user class method called on a Class-object value: dispatch on the boxed
# class id (models Rails' `model.table_name`).
class A
  def self.table_name; "as"; end
end
class B
  def self.table_name; "bs"; end
end
class Rel
  def initialize(model); @model = model; @table = model.table_name; end
  def table; @table; end
end
puts Rel.new(A).table
puts Rel.new(B).table

# a class method WITH an argument, dispatched through an ivar-held Class value
# (the row-hydration half of the Relation pattern) -- the argument is
# evaluated once even across the per-class switch.
class Row
  attr_reader :v
  def initialize(v); @v = v; end
  def self.build(row); new("Row:#{row}"); end
end
class Col
  attr_reader :v
  def initialize(v); @v = v; end
  def self.build(row); new("Col:#{row}"); end
end
class Hydrator
  def initialize(model); @model = model; end
  def hydrate(rows); rows.map { |r| @model.build(r) }; end
end
puts Hydrator.new(Row).hydrate(["x"]).first.v
puts Hydrator.new(Col).hydrate(["y"]).first.v
