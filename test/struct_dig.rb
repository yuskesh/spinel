# Struct#dig is variadic. The first key is member access (by symbol /
# string name or integer position); any further keys dig recursively
# into the member's value. Previously dig fell through to an unresolved
# nil and emitted a C type error.
Person = Struct.new(:name, :age)
p = Person.new("Alice", 30)
puts p.dig(:name)
puts p.dig(:age)
puts p.dig(0)
puts p.dig(1)

# nested dig: a member holding a hash is dug recursively
Config = Struct.new(:name, :address)
c = Config.new("server", { "country" => "JP", "city" => "Tokyo" })
puts c.dig(:address, "country")
puts c.dig(:address, "city")
