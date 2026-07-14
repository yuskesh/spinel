# A receiverless call to an instance_exec trampoline splices the literal
# block with self rebound (previously the block was silently dropped), and
# a parenthesized value-tail keeps its leading statements ordered ahead of
# the spliced block body.
class App
  def initialize = (@o = [])
  def go(&blk) = instance_exec(&blk)
  def leaf(x) = (@o << x)
  def render = (go { leaf("a") }; @o)
end
p App.new.render

class Builder
  def initialize = (@o = [])
  def el(tag, &blk) = (@o << tag; instance_exec(&blk))
  def leaf(x) = (@o << x)
  def render = (el("col") { leaf("a") }; @o)
end
p Builder.new.render

class WithArgs
  def initialize = (@sum = 0)
  def run(x, &blk) = instance_exec(x, &blk)
  def add(n) = (@sum += n)
  def total = (run(5) { |v| add(v * 2) }; @sum)
end
p WithArgs.new.total

class Both
  def initialize = (@log = [])
  def outer(&blk) = instance_exec(&blk)
  def mark(t) = (@log << t)
  def drive
    outer { mark(:first) }
    self.outer { mark(:second) }
    @log
  end
end
p Both.new.drive
