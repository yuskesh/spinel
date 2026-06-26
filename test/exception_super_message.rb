# A user Exception subclass with a custom initialize that forwards to super must
# carry the resolved message when raised WITHOUT an explicit message argument.
# Previously `raise E` skipped the custom initialize and defaulted the message to
# the class name, silently losing the default/super-built message.

# Bare `super` forwards the defaulted param.
class E < StandardError
  def initialize(m = "def"); super; end
end
begin; raise E; rescue => e; p e.message; end          # "def"
begin; raise E, "x"; rescue => e; p e.message; end      # "x"
begin; raise E.new; rescue => e; p e.message; end       # "def"
begin; raise E.new("y"); rescue => e; p e.message; end  # "y"

# super("...") with interpolation, raised argless.
class F < StandardError
  def initialize(m = "dd"); super("wrapped: #{m}"); end
end
begin; raise F; rescue => e; p e.message; end           # "wrapped: dd"

# No custom initialize: message defaults to the class name (unchanged fast path).
class G < StandardError; end
begin; raise G; rescue => e; p e.message; end           # "G"

# Inherited user initialize from a user parent.
class Base < StandardError
  def initialize(m = "base-def"); super; end
end
class Sub < Base; end
begin; raise Sub; rescue => e; p e.message; end          # "base-def"

# Custom initialize that also sets an ivar, raised argless.
class H < StandardError
  def initialize(m = "oops", code = 42); @code = code; super(m); end
end
begin; raise H; rescue => e; p e.message; end            # "oops"

# Built-in raises are unaffected.
begin; raise StandardError; rescue => e; p e.message; end  # "StandardError"
begin; raise "boom"; rescue => e; p e.message; end         # "boom"
