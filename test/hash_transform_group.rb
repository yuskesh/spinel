# Hash#flat_map / #filter_map / #partition / #group_by build collections from
# the |key, value| pairs: flat_map concatenates the per-entry arrays,
# filter_map keeps truthy block values, partition splits into matching and
# remaining pairs, and group_by buckets pairs by the block value. Helpers are
# monomorphic so the runtime walk is exercised without widening to poly.

def flat_kv(h)   = h.flat_map { |k, v| [k, v] }
def flat_dup(h)  = h.flat_map { |k, v| [v, v] }
def fmap_big(h)  = h.filter_map { |k, v| v if v > 1 }
def fmap_key(h)  = h.filter_map { |k, v| k }
def part_even(h) = h.partition { |k, v| v.even? }
def part_str(h)  = h.partition { |k, v| v > 1 }
def group_even(h) = h.group_by { |k, v| v.even? }
def group_mod(h)  = h.group_by { |k, v| v % 3 }

p flat_kv({ a: 1, b: 2 })
p flat_dup({ a: 1, b: 2 })
p fmap_big({ a: 1, b: 2, c: 3 })
p fmap_key({ a: 1, b: 2 })
p part_even({ a: 1, b: 2, c: 3, d: 4 })
p part_str({ "a" => 1, "b" => 2, "c" => 3 })
p group_even({ a: 1, b: 2, c: 3 })
p group_mod({ a: 1, b: 2, c: 3, d: 4 })
