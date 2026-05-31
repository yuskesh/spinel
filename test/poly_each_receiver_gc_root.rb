# The poly (runtime-dispatch) `each` codegen stored the iterated
# collection in an unrooted temp:
#
#     sp_RbVal _t = <receiver>;   // e.g. a method that builds an array
#     for (...) sp_PtrArray_get((sp_PtrArray *)_t.v.p, i) ...
#
# When the receiver is a freshly built heap array referenced only by
# that temp (the `@article.comments` shape -- a new PtrArray of records)
# and the loop body allocates, a GC firing mid-iteration freed the
# unrooted array; the next element fetch then read a dangling pointer,
# surfacing as a SIGSEGV in sp_obj_cls_id_of under load. The GC.start
# inside the loop makes that collection deterministic here.
class Box
  def initialize(name); @name = name; end
  def name; @name; end
  def rename(x); @name = x; end   # self-mutating -> heap, not a value type
end

class Bag
  def initialize; @items = []; end
  def add(b); @items.push(b); end

  # Build a FRESH array on every call; the ternary widens the return
  # type to poly so `each` takes the runtime-dispatch codegen path.
  def items
    fresh = []
    @items.each { |x| fresh.push(x) }
    fresh.empty? ? [] : fresh
  end
end

bag = Bag.new
5.times { |i| bag.add(Box.new("item-" + i.to_s)) }

out = []
bag.items.each do |b|
  GC.start          # collect mid-iteration; unrooted receiver array freed here
  out.push(b.name)
end
puts out.join(",")
