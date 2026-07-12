p (1..10).lazy.drop_while { |x| x < 3 }.to_a
p (1..10).lazy.each_slice(2).to_a
p (1..10).lazy.each_slice(3).to_a
p (1..10).lazy.map { |x| x * 2 }.drop_while { |x| x < 9 }.to_a
p (1..10).lazy.select { |x| x.even? }.each_slice(2).to_a
p (1..10).lazy.each_slice(4).first(2)
