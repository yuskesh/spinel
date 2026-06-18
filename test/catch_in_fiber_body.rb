# catch/throw used inside a fiber body (and a poly-valued catch) must compile.
r = catch(:done) do
  f = Fiber.new do
    catch(:inner) { Fiber.yield }   # poly block value (Fiber.yield); :inner never thrown
  end
  f.resume
  throw :done, "main-throw"
end
puts r
# poly-typed catch whose throw arm fires with an int
v = catch(:x) do
  [1].each { |i| throw :x, 99 }
  Fiber.new { Fiber.yield }
  nil
end
p v
