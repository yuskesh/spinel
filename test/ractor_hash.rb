# str=>int hash
r = Ractor.new do
  h = Ractor.receive
  Ractor.yield(h["a"] + h["b"])
end
r << {"a" => 10, "b" => 32}
puts r.take

# sym=>poly hash
r2 = Ractor.new do
  h = Ractor.receive
  Ractor.yield(h[:name].to_s + "/" + h[:n].to_s)
end
r2 << {name: "spinel", n: 42}
puts r2.take

# hash as spawn arg
r3 = Ractor.new({"x" => 5, "y" => 9}) do |cfg|
  Ractor.yield(cfg["x"] * cfg["y"])
end
puts r3.take
