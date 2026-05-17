# Symbol#inspect literal prefix must use SPL marker so sp_str_byte_len's
# s[-1] marker check is well-defined (avoids -O2 __builtin_trap).

puts :hello.inspect          # :hello

sym = :world
puts sym.inspect             # :world

puts "done"
