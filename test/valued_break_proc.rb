# break inside a lambda returns from the LAMBDA (CRuby); break inside an
# explicitly created non-lambda proc raises LocalJumpError "break from
# proc-closure" when called -- even while the creating iterator is live.
l = -> { break 9 }
p l.call
p lambda { break }.call

# lambda break runs intervening ensures on its way out
l2 = -> { begin; break 3; ensure; puts "le"; end }
p l2.call

# a lambda created and broken inside an iterator stays lambda-local
p([1].map { l3 = -> { break 42 }; l3.call })

# an escaped proc raises when called after its context is gone
def orphan; proc { break 1 }; end
begin
  orphan.call
rescue LocalJumpError => e
  puts "#{e.class}: #{e.message}"
end

# ...and CRuby raises for an explicit proc even while the iterator is live
[1, 2].each do |x|
  pr = proc { break :pb }
  begin
    pr.call
  rescue LocalJumpError => e
    puts "live: #{e.message}"
  end
end
puts "done"
