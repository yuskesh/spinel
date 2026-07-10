# A bare `rescue` matches only StandardError and its subclasses. An Exception
# or NotImplementedError (which is a ScriptError, not a StandardError) falls
# through to a later `rescue Exception`.
def br(e)
  begin
    raise e
  rescue => x
    "bare"
  rescue Exception => x
    "typed"
  end
end

p br(NotImplementedError.new)   # "typed"  (NotImplementedError < ScriptError)
p br(Exception.new)             # "typed"
p br(ArgumentError.new)         # "bare"   (ArgumentError < StandardError)
p br(RuntimeError.new)          # "bare"
p br(KeyError.new)              # "bare"   (KeyError < IndexError < StandardError)

# NotImplementedError is NOT a StandardError.
p NotImplementedError.new.is_a?(StandardError)   # false
p NotImplementedError.new.is_a?(ScriptError)     # true
