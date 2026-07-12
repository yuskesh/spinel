# Blockless String#gsub(/re/) is an Enumerator over the matches, and
# each_line(sep) / lines(sep) split on an arbitrary separator (kept on
# each segment) like CRuby.
p "aaa".gsub(/a/).to_a
p "a1b2".gsub(/\d/).to_a
p "1-2-3".each_line("-").to_a
p "1-2-3".lines("-")
e = "x,y".each_line(",")
p e.to_a
