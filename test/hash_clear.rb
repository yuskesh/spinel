h1 = { a: 1, b: 2, c: 3 }
h1.clear
puts h1.size
puts h1.empty?

h2 = { "a" => 1, "b" => 2 }
h2.clear
puts h2.size

h3 = { x: "one", y: "two" }
h3.clear
puts h3.size

h4 = { a: 1, b: "x" }
h4.clear
puts h4.size

h5 = { "a" => 1, "b" => "x" }
h5.clear
puts h5.size

h6 = { "a" => "one", "b" => "two" }
h6.clear
puts h6.size

h7 = { 1 => "one", 2 => "two" }
h7.clear
puts h7.size

h8 = { 1 => "one", 2 => 2 }
h8.clear
puts h8.size

h9 = { "a" => "one", 2 => 2 }
h9.clear
puts h9.size

# After clear, can write again — slot reset works
h_rw = { a: 1, b: 2 }
h_rw.clear
h_rw[:z] = 99
puts h_rw[:z]
puts h_rw.size
