# Enumerator::Lazy over a range returned from a method.
def src; (1..9); end
p src.lazy.map { |x| x }.first(3)
p src.lazy.map { |x| x * 2 }.first(4)
