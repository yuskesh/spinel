# Issue #667: spinel is an AOT compiler with static dispatch; method
# redefinition is a documented subset limitation. The analyzer detects
# the case in append_cls_meth and emits a stderr warning, then follows
# "last def wins" semantics (matching class reopen for new methods).
# This test verifies the program compiles and runs with the warning
# emitted; full CRuby semantics ("original" then "redefined") would
# require source-order call-site versioning, which is a separate
# multi-day effort.

class Foo
  def test
    "original"
  end
end

f = Foo.new
puts f.test

class Foo
  def test
    "redefined"
  end
end

puts f.test
