# binding.local_variable_get(:name) with a literal symbol naming an in-scope
# local is the only way to read a reserved-word parameter. It has a static
# answer (the value of that local), so it lowers to a direct read.

# reading a reserved-word keyword parameter (`then`)
def read_reserved(then:)
  binding.local_variable_get(:then).inspect
end
puts read_reserved(then: :tick)

# an ordinary parameter name
def read_named(name:)
  binding.local_variable_get(:name)
end
p read_named(name: "hello")

# another reserved word (`if`) as a parameter
def read_if(if:)
  binding.local_variable_get(:if).to_s
end
puts read_if(if: 99)

# a plain local (not a parameter)
def read_local
  count = 41 + 1
  binding.local_variable_get(:count)
end
p read_local

# top-level local
x = [1, 2, 3]
p binding.local_variable_get(:x)
