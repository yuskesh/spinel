# Hash#sum / #count / #all? / #any? with a block reduce the entries to a scalar:
# sum accumulates the block value, count tallies truthy results, all?/any?
# short-circuit to a boolean. Helpers are monomorphic so the runtime walk is
# exercised without widening the parameter to poly.

def sum_v(h)     = h.sum { |k, v| v }
def sum_init(h)  = h.sum(10) { |k, v| v }
def sum_float(h) = h.sum { |k, v| v * 1.0 }
def count_gt(h)  = h.count { |k, v| v > 1 }
def count_key(h) = h.count { |k, v| k == :a }
def all_pos(h)   = h.all? { |k, v| v > 0 }
def all_big(h)   = h.all? { |k, v| v > 1 }
def any_big(h)   = h.any? { |k, v| v > 2 }
def any_mid(h)   = h.any? { |k, v| v > 1 }

p sum_v({ a: 1, b: 2, c: 3 })
p sum_init({ a: 1, b: 2 })
p sum_float({ a: 1, b: 2 })
p count_gt({ a: 1, b: 2, c: 3 })
p count_key({ a: 1, b: 2 })
p all_pos({ a: 1, b: 2 })
p all_big({ a: 1, b: 2 })
p any_big({ a: 1, b: 2 })
p any_mid({ a: 1, b: 2 })
