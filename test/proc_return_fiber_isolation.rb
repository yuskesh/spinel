# Proc-return home chains are per-fiber: interleaving non-local returns across
# the main fiber and a Fiber must not corrupt either chain. Each `worker` call
# is a self-contained non-local return; the fiber yields between them while the
# main fiber also performs its own returns.
def worker(n)
  proc { return n * 10 }.call
end
results = []
f = Fiber.new do
  results << worker(1)
  Fiber.yield
  results << worker(2)
end
results << worker(3)
f.resume
results << worker(4)
f.resume
results << worker(5)
p results
