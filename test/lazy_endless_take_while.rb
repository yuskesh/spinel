p (1..Float::INFINITY).lazy.take_while { |x| x < 5 }.to_a
p (1..).lazy.take_while { |x| x * x < 30 }.to_a
p (1..Float::INFINITY).lazy.map { |x| x * 3 }.take_while { |x| x < 20 }.to_a
