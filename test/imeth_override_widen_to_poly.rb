# #567 (Sam Ruby). Sibling of #563. When an override widens a
# value param to sp_RbVal (poly) because its call sites pass
# mixed types, but the parent's same param stays scalar, the C
# function signatures of base and override disagree. The
# override-dispatch gate cls_imeth_override_ptypes_match
# rejected the family, so a `self[k] = v` inside a parent body
# fell through to the static call on the parent's raise stub
# rather than dispatching to the override.
#
# Fix: post-fixpoint, unify the family's ptypes at any slot
# where at least one member is "poly" so all member signatures
# agree. The cls_id switch then fires and routes to the
# concrete override.

class Base
  def []=(name, value); raise NotImplementedError; end
  def fill; self[:title] = "Hello"; end
end

class Article < Base
  attr_accessor :id, :title
  def []=(name, value)
    case name
    when :id then @id = value
    when :title then @title = value
    end
  end
end

a = Article.new
a.id = 0
a.title = ""
a[:id] = 42
a[:title] = "Other"
a.fill
puts a.title.inspect
