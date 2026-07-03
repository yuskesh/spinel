def id(x); x; end
begin
  id({}).no_such_method_xyz
  puts "not reached"
rescue NoMethodError => e
  puts e.class
end
begin
  r = id(nil).no_such_method_abc
  puts r.inspect
rescue NoMethodError => e
  puts e.class
end
puts "after"
