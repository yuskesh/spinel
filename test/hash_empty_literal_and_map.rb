# Hash conformance (KieranP #2335,#2342,#2343)
p({}.nil?)                          # #2335 object query on empty hash literal
p({}.frozen?)
p({}.empty?)
p({}.is_a?(Hash))
p({}.is_a?(Array))
p(Hash[])                           # #2342 Hash[] with no arguments
p({ a: 1, b: 2 }.map { |k, v| nil })       # #2343 all-nil block map
p({ a: 1, b: 2 }.collect { |k, v| nil })
p({ a: 1, b: 2 }.map { |k, v| k })
