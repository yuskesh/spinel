# instance_exec in a method defined in an INCLUDED module must compile: the
# block's self rebinds to the includer (CRuby inserts a per-includer iclass and
# runs the block on the receiver), so bare calls inside the block dispatch on the
# includer and ivars type against the includer's slot -- not the module's.
module Builders
  def collect(&blk) = (instance_exec(&blk); @o)
  def leaf(x) = (@o << x)
end

class App
  include Builders
  def initialize = (@o = [])
end

# A second includer: each includer gets its own clone, so self resolves per
# includer (App vs Widget), not to the shared module.
class Widget
  include Builders
  def initialize = (@o = [])
end

p App.new.collect { leaf("a"); leaf("b") }
p Widget.new.collect { leaf("w") }
