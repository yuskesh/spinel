# Float#round/ceil/floor/truncate return type matches CRuby's
# value-based rule for a literal ndigits: no-arg and ndigits <= 0 return
# Integer, ndigits > 0 returns Float. (A non-literal ndigits stays
# Float.) See docs/float-rounding.md.
puts 3.5.round(0).inspect
puts 3.5.round.inspect
puts 3.5.round(1).inspect
puts 3.5.round(2).inspect

puts 3.7.ceil(0).inspect
puts 3.7.ceil.inspect
puts 3.7.ceil(1).inspect

puts 3.7.floor(0).inspect
puts 3.7.floor.inspect

puts 3.7.truncate(0).inspect
puts 3.7.truncate.inspect
