# Kernel#fail is an exact alias of Kernel#raise: a bare message raises a
# RuntimeError, an explicit class + message raises that class, and a bare `fail`
# inside a rescue re-raises the current exception.

# message form -> RuntimeError
begin
  fail "boom"
rescue => e
  puts "#{e.class}: #{e.message}"   # RuntimeError: boom
end

# explicit class + message
class MyError < StandardError; end
begin
  fail MyError, "bad input"
rescue => e
  puts "#{e.class}: #{e.message}"   # MyError: bad input
end

# explicit class only -> default message is the class name
begin
  fail MyError
rescue => e
  puts "#{e.class}: #{e.message}"   # MyError: MyError
end

# bare `fail` inside a rescue re-raises the current exception
begin
  begin
    raise "original"
  rescue
    fail
  end
rescue => e
  puts "#{e.class}: #{e.message}"   # RuntimeError: original
end

# raise still works alongside fail (regression)
begin
  raise ArgumentError, "still works"
rescue => e
  puts "#{e.class}: #{e.message}"   # ArgumentError: still works
end
