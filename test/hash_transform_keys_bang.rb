# Hash#transform_keys! / transform_values! on a direct literal (KieranP #2355)
p({ a: 1, b: 2 }.transform_keys!(&:to_s))       # key type sym -> str
p({ a: 1, b: 2 }.transform_keys! { |k| k.to_s })
p({ a: 1, b: 2 }.transform_values!(&:to_s))
p({ a: 1, b: 2 }.transform_keys! { |k| k })
h = { a: 1, b: 2 }
h.transform_values! { |v| v * 10 }               # variable form (same variant)
p h
