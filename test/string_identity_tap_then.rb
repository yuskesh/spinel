s = "hello"
puts s.itself.equal?(s)
puts s.tap { |x| x }.equal?(s)
puts s.then { |x| x }.equal?(s)
