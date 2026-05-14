idx = 0
if idx
  puts "local zero truthy"
else
  puts "local zero falsey"
end

lookup = { "zero" => 0 }
value = lookup["zero"]
if value
  puts "hash zero truthy"
else
  puts "hash zero falsey"
end

width = 0 || 800
puts width
