# defined?(yield) must reflect whether the current method actually received a
# block: nil when called without one, "yield" when a block is present.
def f
  defined?(yield)
end
p f                 # no block -> nil
p f { 1 }           # block    -> "yield"

# routed through a helper that conditionally yields, to exercise the lowered
# __yblk__ runtime path rather than a fully-inlined block.
def g(&blk)
  defined?(yield)
end
p g                 # nil
p g { 42 }          # "yield"
