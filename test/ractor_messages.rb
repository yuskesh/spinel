# Ractor message codec (RFC, Milestone 2): deep-copy String / Symbol / Array
# across the heap boundary. Uses `<<` and variable sends; a bare
# `r.send("literal")` is parsed as a reflective send (whole-program behaviour),
# so messages go via `<<` or a local.

# String
r = Ractor.new do
  s = Ractor.receive
  Ractor.yield(s + " world")
end
r << "hello"
puts r.take

# Symbol (re-interned by name in the receiver)
r2 = Ractor.new do
  Ractor.yield(Ractor.receive.to_s + "!")
end
r2 << :greetings
puts r2.take

# Heterogeneous (poly) array
r3 = Ractor.new do
  a = Ractor.receive
  Ractor.yield(a[0] + a[2])
end
r3 << [10, "x", 32]
puts r3.take

# Integer array
r4 = Ractor.new do
  nums = Ractor.receive
  Ractor.yield(nums[0] + nums[4])
end
r4 << [1, 2, 3, 4, 5]
puts r4.take

# send() with a (non-literal) variable reaches the Ractor mailbox
r5 = Ractor.new do
  Ractor.yield(Ractor.receive * 3)
end
m = 14
r5.send(m)
puts r5.take
