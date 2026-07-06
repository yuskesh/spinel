# Enumerator::Lazy over a range held in a local variable.
r = (1..9)
p r.lazy.map { |x| x * x }.first(3)
p r.lazy.select { |x| x.even? }.first(2)
