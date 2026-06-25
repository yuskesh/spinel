# A Fiber value answers the reflection protocol like any object: #class names
# it "Fiber", and is_a?/kind_of?/instance_of?, the `===` case operator, and a
# case/when all resolve against Fiber and its ancestors.
x = Fiber.new { Fiber.yield 1 }

p x.class
p x.class.name
puts x.class.to_s

p x.is_a?(Fiber)
p x.is_a?(Object)
p x.kind_of?(Fiber)
p x.instance_of?(Fiber)
p x.instance_of?(Object)   # instance_of? is exact: false for a superclass

p(Fiber === x)
puts(case x when Fiber then "is fiber" else "no" end)

# Fiber.current is also a Fiber.
p Fiber.current.is_a?(Fiber)
