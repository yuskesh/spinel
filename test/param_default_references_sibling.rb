# A parameter default that references an earlier parameter (positional or
# keyword) must be evaluated with that parameter bound, exactly as CRuby
# evaluates defaults left-to-right in the callee.

def kw(a:, b: a * 2)
  [a, b]
end
p kw(a: 5)
p kw(a: 5, b: 100)

def pos(a, b = a * 2)
  [a, b]
end
p pos(5)
p pos(5, 100)

# a chain of defaults, each referencing the previous
def chain(a, b = a + 1, c = b * 2)
  [a, b, c]
end
p chain(10)
p chain(10, 20)
p chain(10, 20, 30)

def kwchain(x:, y: x + 1, z: y + 1)
  [x, y, z]
end
p kwchain(x: 1)
p kwchain(x: 1, z: 99)

# string-typed default referencing a string param
def greet(name, msg = name + "!")
  [name, msg]
end
p greet("hi")
p greet("hi", "yo")

# a keyword default referencing a positional param
def mix(a, b: a * 3)
  [a, b]
end
p mix(4)
p mix(4, b: 40)
