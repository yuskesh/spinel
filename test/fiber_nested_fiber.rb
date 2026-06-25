# A Fiber / Enumerator.new body may itself create another Fiber / Enumerator.
# Each body lowers to its own file-scope C function. Previously the inner
# body was emitted into the shared g_procs buffer while the outer body was
# still being written there, so the inner function definition landed inside
# the outer one ("invalid storage class for function"). The body is now built
# in a local buffer and appended after any inner definitions.

outer = Fiber.new do
  inner = Fiber.new do
    Fiber.yield 10
  end
  Fiber.yield inner.resume
  Fiber.yield 20
end
p outer.resume
p outer.resume

# Inner fiber capturing an outer value local.
base = 100
cap = Fiber.new do
  inner = Fiber.new do
    Fiber.yield base + 1
    Fiber.yield base + 2
  end
  Fiber.yield inner.resume
  Fiber.yield inner.resume
  Fiber.yield base
end
p cap.resume
p cap.resume
p cap.resume

# Three levels deep.
f3 = Fiber.new do
  g = Fiber.new do
    h = Fiber.new do
      Fiber.yield 1
    end
    Fiber.yield h.resume + 10
  end
  Fiber.yield g.resume + 100
end
p f3.resume

# A nested Enumerator.new generator inside a Fiber.
ef = Fiber.new do
  e = Enumerator.new do |y|
    y << 7
    y << 8
  end
  Fiber.yield e.next
  Fiber.yield e.next
end
p ef.resume
p ef.resume

# Nested block AND nested fiber combined.
combo = Fiber.new do
  acc = 0
  2.times do |i|
    inner = Fiber.new do
      Fiber.yield i * 10
    end
    acc += inner.resume
    Fiber.yield acc
  end
end
p combo.resume
p combo.resume
