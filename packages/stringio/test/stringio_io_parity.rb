# StringIO: chainable <<, gets(sep), seek whence, variadic print/puts
# (arrays flattened), and the readline/readlines reader family.
require "stringio"

io = StringIO.new(+"")
p (io << "ab").class          # StringIO
(io << "cd") << "ef"
p io.string                   # "abcdef"

g = StringIO.new("a-b-c")
p g.gets("-")                 # "a-"
p g.gets("-")                 # "b-"
p g.gets("-")                 # "c"
p g.gets("-")                 # nil

s = StringIO.new("12345678")
s.seek(-2, IO::SEEK_END)
p s.pos                       # 6
s.seek(2, IO::SEEK_SET)
p s.pos                       # 2
s.seek(1, IO::SEEK_CUR)
p s.pos                       # 3
begin
  s.seek(-99, IO::SEEK_CUR)
rescue => e
  puts "#{e.class}: #{e.message}"
end

pr = StringIO.new(+"")
pr.print(1, 2, 3)
p pr.string                   # "123"
pr.print(" x=", 4.5)
p pr.string                   # "123 x=4.5"

pu = StringIO.new(+"")
pu.puts([1, 2, 3])
p pu.string                   # "1\n2\n3\n"
pu.puts("done\n")
p pu.string                   # "1\n2\n3\ndone\n"
pu2 = StringIO.new(+"")
pu2.puts("a", "b")
pu2.puts(nil)
p pu2.string                  # "a\nb\n\n"

r = StringIO.new("line1\nline2\n")
p r.readline                  # "line1\n"
p r.readline                  # "line2\n"
begin
  r.readline
rescue EOFError => e
  puts "#{e.class}: #{e.message}"
end
r.rewind
p r.readlines                 # ["line1\n", "line2\n"]
