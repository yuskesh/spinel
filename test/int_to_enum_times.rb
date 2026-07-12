# Integer#to_enum(:times) / #enum_for(:times) (and String#to_enum(:each_byte))
# retarget to the blockless builtin form, so .to_a / .map materialize.
