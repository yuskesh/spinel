# #523. Sibling to #516. When a subclass inherits a class method
# whose body calls another class method (`self.last` calls bare
# `all`), spinel correctly monomorphized the call (`sp_Sub_cls_all`
# is invoked) but the surrounding inference of the call's result
# type used the parent's signature -- the local slot for the
# returned array was typed `sp_IntArray *` (from Base.all's empty
# `[]` literal) while `sp_Sub_cls_all` returned `sp_PtrArray *`,
# triggering an `incompatible pointer types` C warning and an
# `int-from-pointer` error on the subsequent `[-1]` dispatch.
#
# Root cause: analyze's walk_and_cache wrote a per-AST-node-id
# type cache, and inherited cmethod bodies share their AST node
# ids across subclass copies. The first walker (typically the
# defining class) populated the cache; later walkers' recomputes
# under different @current_class_idx were short-circuited by the
# cache hit at the top of infer_type.
#
# Fix (analyze): walk_and_cache invalidates the cache before
# recompute so the second walker overwrites, and skip_cache is
# extended to bare CallNodes that resolve to a sibling cmeth on
# the current class. Fix (codegen): infer_type's CallNode arm now
# mirrors analyze's @cls_cmeth_returns lookup for bare cmeth
# calls so the cache-miss path picks up the subclass's override
# at emit time.

class Base
  def self.all
    []
  end

  def self.last
    records = all
    records.empty? ? nil : records[-1]
  end
end

class Sub < Base
  def self.all
    [Object.new]
  end
end

# Sub.last (inherited from Base) sees Sub.all's PtrArray return
# type now, so `records.empty?` and `records[-1]` dispatch into
# the PtrArray path instead of the IntArray path.
r = Sub.last
if r != nil
  puts "got object"
else
  puts "nil"
end
