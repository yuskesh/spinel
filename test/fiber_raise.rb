# Fiber#raise injects an exception at the fiber's suspension point. The argument
# forms mirror Kernel#raise. A handled raise lets the fiber continue; an
# unhandled one propagates to the caller of #raise.

# Rescued inside the fiber: the suspended Fiber.yield raises, the rescue runs,
# and the fiber yields again / returns normally.
f = Fiber.new do
  begin
    Fiber.yield 1
  rescue => e
    Fiber.yield "rescued: #{e.message}"
  end
  :done
end
p f.resume            # 1
p f.raise("boom")     # "rescued: boom"
p f.resume            # :done

# (Class, message), unhandled: propagates to the #raise caller; fiber dies.
g = Fiber.new { Fiber.yield 1 }
g.resume
begin
  g.raise(RuntimeError, "up")
rescue => e
  puts "caught #{e.class}: #{e.message}"
end
p g.alive?

# A class with no message.
h = Fiber.new { Fiber.yield 1 }
h.resume
begin
  h.raise(ArgumentError)
rescue ArgumentError
  puts "caught ArgumentError"
end

# A pre-built exception object.
i = Fiber.new { Fiber.yield 1 }
i.resume
begin
  i.raise(TypeError.new("obj msg"))
rescue => e
  puts "caught #{e.class}: #{e.message}"
end

# raise with no argument -> RuntimeError.
j = Fiber.new { Fiber.yield 1 }
j.resume
begin
  j.raise
rescue RuntimeError
  puts "caught bare RuntimeError"
end
