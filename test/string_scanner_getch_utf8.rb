require "strscan"
# getch advances by one UTF-8 character, not one byte.
s = StringScanner.new("café")
puts s.getch
puts s.getch
puts s.getch
puts s.getch
puts s.eos?
puts s.pos

# pos tracks byte offsets across multibyte chars; unscan rewinds the char.
t = StringScanner.new("añb")
puts t.getch
puts t.getch
puts t.pos
t.unscan
puts t.pos
puts t.getch
puts t.getch
puts t.eos?

# 3-byte character.
u = StringScanner.new("€x")
puts u.getch
puts u.pos
puts u.getch
