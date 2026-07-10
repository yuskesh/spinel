# Enumerator terminals over slice/chunk enumerators, string byte enumerator,
# boolean chunk keys, zip with a Range argument, and lazy take(n).to_a.
p "hi".each_byte.to_a
p [1, 2, 3, 4].each_slice(2).count
p [1, 2, 3, 4].each_slice(2).to_h
p [1, 2, 3].each_cons(2).count
p [1, 1, 2, 3].chunk { |x| x }.map { |k, v| k }
p [1, 2, 3, 4].chunk { |x| x.even? }.to_a
p [1, 2, 3].zip(4..6)
p [1, 2].zip([3, 4], 5..6)
p (1..Float::INFINITY).lazy.map { |x| x * x }.take(3).to_a
p [1, 2, 3, 4].take(2).to_a
