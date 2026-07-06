# $! is the exception object the innermost active rescue is handling (nil
# outside one): same identity as the rescue variable and the raised object.
e = StandardError.new 'foo'
begin
  raise e
rescue => err
  puts err.message
  puts $!.message
  puts $!.equal?(err)
  puts $!.equal?(e)
end
p $!
begin
  raise ArgumentError, "inner"
rescue
  puts $!.message
  begin
    raise TypeError, "deep"
  rescue
    puts $!.message
  end
  puts $!.message
end
p $!
