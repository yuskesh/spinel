# An exception's #class is a real Class object: it inspects bare (not quoted)
# inside a container, renders as its name via to_s/name, and compares by identity.
begin
  raise ArgumentError, "x"
rescue => e
  p [e.class, e.message]       # [ArgumentError, "x"]
  p e.class                    # ArgumentError
  puts e.class.to_s            # ArgumentError
  puts e.class.name            # ArgumentError
  p e.class == ArgumentError   # true
  p e.class == RuntimeError    # false
end

# a builtin default-message exception
begin
  raise "boom"
rescue => e
  p [e.class, e.message]       # [RuntimeError, "boom"]
end

# a user-defined exception subclass reifies to its own Class
class MyErr < StandardError; end
begin
  raise MyErr, "oops"
rescue => e
  p [e.class, e.message]       # [MyErr, "oops"]
  p e.class == MyErr           # true
  puts e.class.name            # MyErr
end

# the class survives flowing through a method and boxing alongside other kinds
def id(v); v; end
begin
  raise TypeError, "nope"
rescue => e
  p [1, id(e).class, :ok]      # [1, TypeError, :ok]
end
