# A stored proc declaring keyword params binds them from the call's kwargs;
# an omitted optional keyword takes its default.
add = proc { |a:, b:| a + b }
p add.call(a: 1, b: 2)
opt = proc { |x:, y: 100| [x, y] }
p opt.call(x: 5)
p opt.call(x: 5, y: 6)
mixed = proc { |n, k:| [n, k] }
p mixed.call(9, k: 3)
# A required keyword omitted from the call raises ArgumentError, as in CRuby.
begin
  add.call(a: 1)
rescue ArgumentError => e
  puts "raised: #{e.message}"
end
