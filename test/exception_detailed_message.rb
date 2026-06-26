# Exception#detailed_message returns "message (ClassName)". (The core method;
# the optional error_highlight gem's source-snippet augmentation, which applies
# to a few argument-style errors, is a separate concern and not reproduced.)

# a RuntimeError carrying a message
begin
  raise "boom"
rescue => e
  p e.detailed_message            # "boom (RuntimeError)"
end

# an explicit class with a message
begin
  raise StandardError, "bad state"
rescue => e
  p e.detailed_message            # "bad state (StandardError)"
end

# no message -> the class name is used as the message
begin
  raise RuntimeError
rescue => e
  p e.detailed_message            # "RuntimeError (RuntimeError)"
end

# a user-defined exception subclass
class MyError < StandardError; end
begin
  raise MyError, "oops"
rescue => e
  p e.detailed_message            # "oops (MyError)"
end

# message and detailed_message side by side
begin
  raise "x"
rescue => e
  p [e.message, e.detailed_message]   # ["x", "x (RuntimeError)"]
end
