# Hash#min_by / #max_by / #find / #detect yield each |key, value| pair and
# return the winning [key, value] pair (or nil when nothing qualifies). Each
# helper is monomorphic (one hash variant) so the runtime walk is exercised
# without widening the parameter to poly.

def min_v(h)     = h.min_by { |k, v| v }
def max_v(h)     = h.max_by { |k, v| v }
def min_v_str(h) = h.min_by { |k, v| v }
def max_len(h)   = h.max_by { |k, v| v.length }
def min_k_int(h) = h.min_by { |k, v| k }
def find_v(h)    = h.find { |k, v| v == 2 }
def find_gone(h) = h.find { |k, v| v > 99 }
def find_k(h)    = h.find { |k, v| k == :b }
def detect_v(h)  = h.detect { |k, v| v.even? }

p min_v({ a: 3, b: 1, c: 2 })
p max_v({ a: 3, b: 1, c: 2 })
p min_v_str({ "a" => 3, "b" => 1 })
p max_len({ a: "xx", b: "y", c: "zzz" })
p min_k_int({ 3 => 30, 1 => 10, 2 => 20 })
p find_v({ a: 1, b: 2, c: 3 })
p find_gone({ a: 1, b: 2 })
p find_k({ a: 1, b: 2 })
p detect_v({ a: 1, b: 4, c: 3 })

# The winning pair is a real array.
pair = max_v({ a: 3, b: 7, c: 5 })
p pair[0]
p pair[1]
