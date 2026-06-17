# ARGF reads the files named in ARGV (here one file passed as an arg),
# line by line; ARGF.filename names the current file.
puts ARGF.filename
ARGF.each_line do |line|
  puts line.chomp.upcase
end
