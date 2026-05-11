# Issue #404 Tier 5. Dynamic `<sp_Class>.new` over a Class-typed
# value. Pre-Tier-5 only the constant-receiver form worked
# (`Foo.new` resolves at codegen time); a Class-typed local
# carrying the same class hit the unresolved-call warning + 0
# emit. The delegated_type / STI shape needs runtime dispatch
# because the candidate class is determined by data.
#
# Coverage:
#   - Pick a class from a hash at runtime, call .new, get a
#     boxed instance, dispatch instance methods via the existing
#     cls_id-keyed poly dispatch.
#   - Multiple classes share the dispatch table.
#   - Only the zero-arg shape is supported in this tier; classes
#     whose initialize requires args fall back to nil from the
#     dispatch (out of scope for the MVP).

class Article
  def kind; "article"; end
end

class Page
  def kind; "page"; end
end

class Comment
  def kind; "comment"; end
end

def make(klass)
  klass.new
end

a = make(Article)
p = make(Page)
c = make(Comment)

puts a.kind
puts p.kind
puts c.kind

# Single dispatch via a Class-typed local variable.
k = Article
obj = k.new
puts obj.kind
