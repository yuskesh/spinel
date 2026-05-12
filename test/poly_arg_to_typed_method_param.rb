# Sibling to #438. A poly local guarded by `is_a?(String)` passed
# as a positional arg to a top-level typed-param method: the call
# site unboxes via `(lv_v).v.s` so the const-char* parameter
# type-checks. Same shape for Integer / Float narrows.

def helper_s(s)
  s.length
end

def encode(v)
  return helper_s(v) if v.is_a?(String)
  -1
end

puts encode("hello")
puts encode(42)

def helper_i(i)
  i * 2
end

def double_if_int(v)
  return helper_i(v) if v.is_a?(Integer)
  0
end

puts double_if_int(7)
puts double_if_int("nope")
