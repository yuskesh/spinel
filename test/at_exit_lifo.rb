# at_exit hooks run in LIFO order after main returns. Spinel used
# to silently drop the block.
at_exit { puts "first registered" }
at_exit { puts "second registered" }
at_exit { puts "third registered" }
puts "main"
