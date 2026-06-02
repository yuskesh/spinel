require "strscan"
# StringScanner#scan / #scan_until assigned to a local must type the local as
# a String (the matched text), not String#scan's array-of-matches.
s = StringScanner.new("42 hi")
m = s.scan(/\d+/)
puts(m + "!")
puts m.to_i + 1
n = s.scan_until(/hi/)
puts n.length
# String#scan (on a real String) still returns an array.
p "a1b2c3".scan(/[a-z]\d/)
