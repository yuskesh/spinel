# A Regexp interpolates into a string as its to_s ("(?-mix:abc)" form),
# both as a literal and through a variable.
r = /abc/x
p "v: #{r}"
p "w: #{/abc/}"
puts "x: #{/a.c/im}"
