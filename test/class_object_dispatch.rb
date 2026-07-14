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
