# #530. `Class.new(kw: val)` (Symbol-keyed kwargs) where the
# class's initialize takes a single positional `attrs` param.
# CRuby binds `attrs = {kw: val}` (the kwargs become a positional
# hash). Spinel previously emitted `sp_Foo_new(0)` because the
# kwarg name didn't match the positional param name -- the
# kwarg got silently dropped and the param defaulted to int.
# `attrs["title"]` inside initialize then segfaulted on the
# NULL deref (on macOS; Linux glibc's zero-init happened to
# hide it).
#
# Fix: when a KeywordHashNode arg's keys match no param name
# and there's still an unfilled positional, treat the whole
# hash as that positional's value. analyze widens the param's
# inferred type to `str_poly_hash` (string keys via sym->s
# interning, poly values for kwarg-value variety) and codegen
# builds the hash from the kwargs at the call site.

class Article
  def initialize(attrs)
    @title = attrs["title"]
    @body  = attrs["body"]
  end
  attr_reader :title, :body
end

a = Article.new(title: "Hello", body: "World")
puts a.title
puts a.body

# Two kwarg call sites with different value shapes -- both flow
# into the same str_poly_hash widening.
b = Article.new(title: "Second", body: "Post")
puts b.title

# Mixed value types in one kwarg call: the str_poly_hash carries
# all the boxed values; consumers unbox at the use site.
class Mixed
  def initialize(opts)
    @s = opts["name"]
    @n = opts["count"]
  end
  attr_reader :s, :n
end

m = Mixed.new(name: "abc", count: 42)
puts m.s
puts m.n.inspect
