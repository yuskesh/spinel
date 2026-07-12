begin; p (1.0..5.0).sum; rescue TypeError => e; puts "sum: #{e.message}"; end
begin; (1.0..5.0).each { |x| p x }; rescue TypeError => e; puts "each: #{e.message}"; end
begin; p (1.0..5.0).to_a; rescue TypeError => e; puts "to_a: #{e.message}"; end
begin; p (1.0..5.0).map { |x| x * 2 }; rescue TypeError => e; puts "map: #{e.message}"; end
begin; p (1.0..5.0).count; rescue TypeError => e; puts "count: #{e.message}"; end
begin; p (1.0..5.0).size; rescue TypeError => e; puts "size: #{e.message}"; end
p (1..5.0).to_a
p (1.0..5.0).include?(2.5)
r = (2.0..9.0)
begin; p r.sum; rescue TypeError => e; puts "local: #{e.message}"; end
p (1.0..5.0).step(2.0).to_a
