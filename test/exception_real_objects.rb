# An exception is a real object: $! and `rescue => e` bind the same object, its
# #class is a real Class (usable in ==, case, printing), and `p` inspects it.

# $! is the actual rescued object (identity), and its class/message read through
begin
  raise "boom"
rescue => e
  p $!.equal?(e)              # true
  p $!.class                  # RuntimeError
  p $!.class == RuntimeError  # true
  p $!.message                # "boom"
  p $!                        # #<RuntimeError: boom>
end

# outside any rescue, $! is nil
p $!                          # nil

# a typed exception class flows through == and drives a case
begin
  raise ArgumentError, "bad arg"
rescue => e
  p e.class                   # ArgumentError
  p e.class == ArgumentError  # true
  label = case e.class.to_s
          when "ArgumentError" then "arg"
          else "other"
          end
  p label                     # "arg"
end

# nested rescue: $! is the innermost handled exception and is identical to its
# `=> e` binding
begin
  raise TypeError, "outer"
rescue => outer
  p outer.class               # TypeError
  begin
    raise IndexError, "inner"
  rescue => inner
    p $!.class                # IndexError
    p $!.equal?(inner)        # true
  end
end

# a user exception subclass keeps its class and custom message through => e
class MyErr < StandardError
  def initialize(code)
    super("code #{code}")
  end
end
begin
  raise MyErr.new(42)
rescue => e
  p e.class                   # MyErr
  p e.message                 # "code 42"
  p $!.equal?(e)              # true
end

# a nil $! (outside any rescue) reports NilClass; equal? against a non-exception
# value is always false, since neither can be the one raised object
p $!.class                    # NilClass
begin
  raise "again"
rescue => e
  p e.equal?(42)              # false
  p e.equal?("again")         # false
end
