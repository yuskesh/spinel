# Phase 1C: built-in exception class `.new("msg")`.
# `RuntimeError.new(...)`, `ArgumentError.new(...)` etc. previously
# emitted an undefined `sp_RuntimeError` type (the analyze inferred
# obj_RuntimeError but no struct backed it). Spinel now models
# built-in exception objects as their message string plus a
# side-channel cls name -- the same convention `rescue => e` uses.
#
# Covers (Phase 1C):
#   - CLS.new("msg") with various exception classes
#   - .class returns the class name (string in spinel)
#   - .message returns the msg
#   - .is_a? walks the hierarchy
#
# Covers (Phase 1D):
#   - raise <exception_object> -- raise a pre-built exc
#   - raise CLS.new("msg") -- raise an inline-constructed exc

err = RuntimeError.new("created")
puts err.class                     # RuntimeError
puts err.message                   # created
puts err.is_a?(RuntimeError)       # true
puts err.is_a?(StandardError)      # true
puts err.is_a?(Exception)          # true

# raise the saved object
begin
  raise err
rescue => e
  puts "caught: #{e.class}: #{e.message}"
end

# raise an inline-constructed exception (1D)
begin
  raise ArgumentError.new("inline ae")
rescue => e
  puts "inline: #{e.class}: #{e.message}"
end

# StandardError subclass with .new
e2 = TypeError.new("bad type")
puts e2.class                      # TypeError
puts e2.message                    # bad type

# Various built-in exception classes
e3 = NoMethodError.new("missing")
puts e3.class                      # NoMethodError
