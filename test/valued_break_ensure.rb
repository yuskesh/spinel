# A break crossing begin/ensure frames runs the ensure bodies on the way out
# (CRuby TAG_BREAK unwinding), then the call yields the break value.
p([1, 2, 3].each { |x| begin; break 1 if x == 2; ensure; puts "e#{x}"; end })

# nested ensures both run, innermost first
p([1, 2].map do |x|
  begin
    begin
      break :deep if x == 2
      x
    ensure
      puts "inner#{x}"
    end
  ensure
    puts "outer#{x}"
  end
end)

# a rescue-only frame between break and the call unwinds cleanly, and a raise
# AFTER the break-carrying loop still lands in the right handler
begin
  r = [1, 2, 3].map { |x| begin; break x * 5 if x == 2; x; rescue; :never; end }
  p r
  raise "boom"
rescue => e
  puts e.message
end

# an exception rescued OUTSIDE a wrapped call, then a later unrelated break
begin
  [1, 2].each { |x| raise "mid" if x == 2 }
rescue => e
  puts e.message
end
p([7, 8].each { |x| break x if x == 8 })

# break through an ensure inside an inlined yield method
def guarded
  begin
    yield
  ensure
    puts "guard"
  end
end
p(guarded { break :gv })

# ensure that allocates while the break value is in flight
p([1, 2].each { |x| begin; break [x, x + 1] if x == 2; ensure; s = "a#{x}" * 3; puts s; end })

# next/redo in a wrapped block keep their meaning
p([1, 2, 3].map { |x| next x * 2 if x == 2; break :b if x == 3; x })
