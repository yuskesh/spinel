# all?/any?/none?/one? on a POLY receiver whose elements are used via a METHOD
# DISPATCH (a struct element's reader) inside the block, not just a direct
# predicate on the block param (which test/poly_predicate_dispatch.rb covers).
# The param stays boxed as poly and `c.key` dispatches by class id -- the shape a
# real object list (e.g. a reconciler's child nodes) takes.
class Item
  def initialize(k)
    @k = k
  end

  def key = @k
end
h = { "items" => [Item.new("a"), Item.new(nil)], "label" => "x" }
items = h["items"]
p items.all? { |c| !c.key.nil? }
p items.any? { |c| c.key.nil? }
p items.none? { |c| c.key.nil? }
p items.one? { |c| c.key.nil? }
