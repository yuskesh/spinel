# Enumerator::Lazy#take(n)/#drop(n) as blockless fused stages, including as the
# first op before any block stage establishes an element type. Receivers are
# routed through method params to exercise the runtime pipeline, not a fold.
def take_only(a) = a.lazy.take(2).to_a
p take_only([1, 2, 3, 4])

def take_strs(a) = a.lazy.take(2).to_a
p take_strs(%w[a b c d])

def drop_only(a) = a.lazy.drop(1).to_a
p drop_only([1, 2, 3, 4])

def drop_then_map(a) = a.lazy.drop(1).map { |x| x * 10 }.to_a
p drop_then_map([1, 2, 3, 4])

def take_then_select(a) = a.lazy.take(4).select { |x| x.odd? }.to_a
p take_then_select([1, 2, 3, 4, 5, 6])

def map_then_take(a) = a.lazy.map { |x| x + 1 }.take(2).to_a
p map_then_take([10, 20, 30, 40])

def drop_take(a) = a.lazy.drop(2).take(2).to_a
p drop_take([1, 2, 3, 4, 5, 6])

def range_take(r) = r.lazy.take(3).to_a
p range_take(1..100)

def range_drop_first(r) = r.lazy.drop(2).first(2)
p range_drop_first(1..100)

def filtered_take(r) = r.lazy.select { |x| x.even? }.take(3).to_a
p filtered_take(1..100)
