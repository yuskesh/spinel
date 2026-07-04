# A statically-false defined?(Const) guard must fold the whole branch away:
# the constant resolves to nothing at compile time, so the branch is dead code.
# Shape from doom's gosu_window YJIT toggle:
#   if defined?(RubyVM::YJIT) && RubyVM::YJIT.enabled?
#     ... RubyVM::YJIT.enable ...
#   end
# The dead branch must neither pull its callees into emission (reachability)
# nor be emitted itself (its body may be uncompilable, like an MRI-only hack).

class YjitShim
end

# Only ever called from dead branches; `frobnicate` does not exist on
# YjitShim, so emitting this body is a hard compile error.
def setup_yjit_toggle
  shim = YjitShim.new
  shim.frobnicate
  puts "toggled"
end

def yjit_toggle
  if defined?(SomeMissingConst) && SomeMissingConst.enabled?
    SomeMissingConst.enable
    setup_yjit_toggle
    puts "on"
  end
  puts "after"
end

def elsif_variant(n)
  if n > 10
    puts "big"
  elsif defined?(AnotherMissingConst)
    setup_yjit_toggle
    puts "activated"
  else
    puts "small"
  end
end

def plain_guard
  if defined?(MissingToo::Nested)
    shim = YjitShim.new
    shim.enable_nested
    puts "unreachable"
  end
  puts "done"
end

# A builtin tail must not mask an unresolved parent: MissingParent does not
# exist, so the whole path is statically false (CRuby agrees: nil).
def qualified_tail_builtin
  if defined?(MissingParent::String)
    shim = YjitShim.new
    shim.frobnicate_qualified
    puts "unreachable"
  end
  puts "qualified done"
end

def nested_missing_root
  if defined?(MissingRoot::Sub::Thing) && MissingRoot::Sub::Thing.on?
    setup_yjit_toggle
    puts "nested on"
  end
  puts "nested done"
end

# Control: a truthy defined? over a real builtin keeps its branch.
def known_guard
  if defined?(String)
    puts "string exists"
  else
    puts "no string"
  end
end

yjit_toggle
elsif_variant(3)
elsif_variant(42)
plain_guard
qualified_tail_builtin
nested_missing_root
known_guard
