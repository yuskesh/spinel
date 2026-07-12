# Integer#to_enum(:times) / #enum_for(:times) (and String#to_enum(:each_byte))
# retarget to the blockless builtin form, so .to_a / .map materialize.
p 3.to_enum(:times).to_a
p 3.enum_for(:times).to_a
p 4.to_enum(:times).map { |i| i * 2 }
p "abc".to_enum(:each_byte).to_a
