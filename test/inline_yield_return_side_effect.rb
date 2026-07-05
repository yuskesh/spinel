def side
  puts "side effect happened"
  42
end

def each_thing
  puts "before"
  yield 1
  return side()
  puts "unreached"
end

each_thing { |x| puts "block #{x}" }
puts "after"
