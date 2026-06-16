# A method whose only use of its &block parameter is to forward it to another
# call (never calling/yielding it directly) must stay a real method and pass
# the block along -- to a yielding target and to an explicit &block target.

def yield_target
  yield 5
end

def block_target(&b)
  b.call(5)
end

def fwd_to_yield(&b)
  yield_target(&b)
end

def fwd_to_block(&b)
  block_target(&b)
end

puts(fwd_to_yield { |x| x * 2 })
puts(fwd_to_block { |x| x * 2 })

# Two-hop forward: the block threads through two forwarding methods.
def hop1(&b)
  fwd_to_yield(&b)
end
puts(hop1 { |x| x + 100 })

# Forwarding an anonymous block parameter.
def fwd_anon(&)
  yield_target(&)
end
puts(fwd_anon { |x| x * 3 })

# A forwarder used at several call sites with different block bodies.
def relay(&b)
  yield_target(&b)
end
puts(relay { |x| x - 1 })
puts(relay { |x| x * x })
