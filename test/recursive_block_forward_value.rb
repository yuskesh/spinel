# A method that forwards its &block down a recursive call and returns
# the result carries the block's value back. The tail is the recursive
# call (its value must be returned, not dropped for side-effect), and
# the governing blk.call is in the base case `return blk.call if ...`.

def countdown(n, &blk)
  return blk.call if n <= 0
  countdown(n - 1) { blk.call }
end

# string block: value survives every recursive sp_proc_call hop and is
# typed string via the base-case return.
puts countdown(2) { "done" }

# int block at the same method
puts countdown(3) { 42 }

# depth 0 (base case taken immediately)
puts countdown(0) { "base" }
