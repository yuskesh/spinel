# #489. Two `class Foo` definitions in the same translation unit
# (whether from two require_relative'd files or a single file with
# re-opened class) previously emitted duplicate C function symbols
# for each repeated method name (sp_Foo_go twice, etc.), failing
# C compile with "redefinition of 'sp_Foo_go'". CRuby's semantics
# are "last definition wins": the second body replaces the first
# for any method with the same name; methods unique to either copy
# survive. Fix: append_cls_meth / append_cls_cmeth detect a matching
# existing entry and replace its row in the parallel arrays instead
# of appending. Same shape repro across require_relative is fully
# covered by this single-file reopen.

class Foo
  def initialize
    @x = 1
  end
  def go
    "first: " + @x.to_s
  end
  def only_first
    "kept"
  end
end

class Foo
  def initialize
    @x = 2
  end
  def go
    "second: " + @x.to_s
  end
  def only_second
    "added"
  end
end

f = Foo.new
puts f.go
puts f.only_first
puts f.only_second
