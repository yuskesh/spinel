class C
  def original = 1
  alias aliased original if true        # statically true: the alias is created

  def greet = "hi"
  alias hello greet unless false        # unless false: also created

  alias parend original if (true)       # parenthesized constant folds too

  # a statically-false guard creates no alias and compiles cleanly. CRuby does
  # not define these either, so they are intentionally never called.
  alias never_made original if false
  alias also_none greet unless true

  # if/elsif: the statically-selected branch's alias is the one registered
  def from_b = "b"
  if false
    alias chosen original
  elsif true
    alias chosen from_b
  end
end

puts C.new.aliased
puts C.new.hello
puts C.new.parend
puts C.new.chosen
puts C.new.original
puts C.new.greet
