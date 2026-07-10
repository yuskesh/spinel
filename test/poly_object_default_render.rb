# A bare Object (boxed with the builtin object class id) renders CRuby's
# #<Object:0xADDR> through the poly to_s/inspect defaults (print/p paths).
puts Object.new.to_s.gsub(/0x[0-9a-f]+/, "0xADDR")
puts Object.new.inspect.gsub(/0x[0-9a-f]+/, "0xADDR")
