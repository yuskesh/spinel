h1 = {"a" => "hello"}
v1 = h1["missing"]
puts v1.nil? ? "nil" : "value"
puts v1.inspect
if h1["missing"]; puts "truthy"; else; puts "falsy"; end
puts h1["a"]

h2 = {a: "hello"}
v2 = h2[:missing]
puts v2.nil? ? "nil" : "value"
if h2[:missing]; puts "truthy"; else; puts "falsy"; end
puts h2[:a]

h3 = {1 => "one", 2 => "two"}
v3 = h3[99]
puts v3.nil? ? "nil" : "value"
if h3[99]; puts "truthy"; else; puts "falsy"; end
puts h3[1]

h4 = Hash.new("none")
puts h4["x"]
h5 = {"a" => "hello"}
h5.default = "fallback"
puts h5["missing"]
