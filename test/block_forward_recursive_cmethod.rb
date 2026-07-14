# A literal block on an implicit-self call to a &block-forwarding recursive
# class method: the block lifts to a real proc (capturing enclosing locals)
# and the &block forward passes the caller's proc through.
class M
  def self.walk(node, &block)
    return block.call(node) if node.is_a?(String)
    node.each { |child| walk(child, &block) }
  end

  def self.go(root)
    out = []
    walk(root) { |n| out << n }
    out.join(",")
  end
end

puts M.go([["a"], "b"])
out2 = []
M.walk([["x", ["y"]], "z"]) { |n| out2 << n }
p out2

# a &block forward CHAIN between class methods carries the block's value
# type through (g's literal block reaches f's blk_ret via the forward)
class N
  def self.f(&block)
    block.call(1)
  end

  def self.g(&block)
    f(&block)
  end
end
p(N.g { |n| n * 3 })
