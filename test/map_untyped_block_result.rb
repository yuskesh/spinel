# An untyped block result (here a read of an unassigned ivar, which is untyped
# and evaluates to nil) collected by map / each_cons must produce nil elements,
# never crash or mis-box. Covers the area of the untyped-into-poly-array boxing
# fix (the RBS-typed reproducer lives in test/rbs-seed/map_untyped_poly.rb).
class Collector
  def plain(arr);           arr.map { |x| @sink };                          end
  def cons(arr);            arr.each_cons(2).map { |pair| @sink };          end
  def cons_with_index(arr); arr.each_cons(2).with_index.map { |p, i| @sink }; end
end

c = Collector.new
p c.plain([1, 2, 3])
p c.cons([1, 2, 3, 4])
p c.cons_with_index([1, 2, 3, 4])
