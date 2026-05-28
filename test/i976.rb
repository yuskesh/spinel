p = Proc.new { |a, b| puts "a=#{a.inspect} b=#{b.inspect}" }
p.call(1)
p.call(1, 2)

q = Proc.new { |x, y, z| puts "x=#{x} y=#{y} z=#{z}" }
q.call(10)
q.call(10, 20)
q.call(10, 20, 30)
