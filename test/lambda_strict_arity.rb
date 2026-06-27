# A non-lambda Proc is lenient: extra args are dropped, no raise.
pr = proc { |x| x }
puts pr.call(1, 2)

# A lambda is strict: the wrong argument count raises ArgumentError
# (matching CRuby). Rescue it so the program completes normally and
# "after" still prints.
puts "before"
f = ->(x) { x }
begin
  f.call(1, 2)
rescue ArgumentError => e
  puts "rescued: #{e.class}"
end
puts "after"
