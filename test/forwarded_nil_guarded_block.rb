# A captured &blk that is nil-guarded (forcing a real proc param) and then
# forwarded into a callee must still reach that callee's yield / block.call.

def inner_yield
  yield 5
end
def inner_call(&b)
  b.call(5)
end
def inner_multi
  yield 3, 4
end

def y_guard(&blk)
  return -1 if blk.nil?
  inner_yield(&blk)
end
def c_guard(&blk)
  return -1 if blk.nil?
  inner_call(&blk)
end
def multi_guard(&blk)
  return -1 if blk.nil?
  inner_multi(&blk)
end

puts y_guard { |x| x * 2 }
puts y_guard
puts c_guard { |x| x + 100 }
puts c_guard
puts multi_guard { |a, b| a + b }
p(y_guard { |x| "got #{x}" })

# a yielding method shared across an Integer and a String forwarder: its yield
# type unifies to poly, but each forwarder's result slot stays concrete.
def s_guard(&blk)
  return "none" if blk.nil?
  inner_yield(&blk)
end
puts s_guard { |x| "v=#{x}" }
puts s_guard
