v = ENV["NO_SUCH_1664"]
begin; v.length; rescue NoMethodError => e; puts "length: #{e.message}"; end
begin; v.upcase; rescue NoMethodError => e; puts "upcase: #{e.message}"; end
begin; v.split(","); rescue NoMethodError => e; puts "split: #{e.message}"; end
begin; v[0]; rescue NoMethodError => e; puts "slice: #{e.message}"; end
begin; v.empty?; rescue NoMethodError => e; puts "empty?: #{e.message}"; end
begin; v.include?("x"); rescue NoMethodError => e; puts "include?: #{e.message}"; end
begin; v + "x"; rescue NoMethodError => e; puts "plus: #{e.message}"; end
begin; "x" + v; rescue TypeError => e; puts "plus_nil_arg: TypeError"; end
begin; v.to_str; rescue NoMethodError => e; puts "to_str: #{e.message}"; end
begin; v.strip; rescue NoMethodError => e; puts "strip: #{e.message}"; end
begin; v.index("x"); rescue NoMethodError => e; puts "index: #{e.message}"; end
begin; v.chars; rescue NoMethodError => e; puts "chars: #{e.message}"; end
puts v.to_s.inspect
puts v.to_i
puts v.dup.nil?
puts v.frozen?
puts v.inspect
a = ["a", v, "b"]
puts a.join("-")
