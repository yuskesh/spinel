# gsub/sub with a String pattern and a block; format/% raising on too-few
# arguments and malformed directives; $! visible inside rescue modifiers;
# a Symbol variable as a block argument (&op) and via to_proc.
p "hello".gsub("l") { |m| m.upcase }
p "hello".sub("l") { |m| m.upcase }
p "hello".gsub("l", &:upcase)
r1 = (format("%d %d %d", 1, 2) rescue "too few")
p r1
begin
  "%,d" % 5
rescue ArgumentError => e
  p e.message
end
begin
  "%z" % 1
rescue ArgumentError => e
  p e.message
end
p format("%05.2f|%x|%c|%%", 3.14159, 255, 65)
p format("%2$s %1$s", "world", "hello")
r = (raise "boom" rescue $!.message)
p r
r2 = ("%z" % 1 rescue $!.message)
p r2
raise "st" rescue puts $!.message
p $!.nil?
op = :upcase
p ["a", "b"].map(&op)
op2 = :+
p [1, 2, 3, 4].reduce(&op2)
a = :upcase
up = a.to_proc
p up.call("hi")
flag = [true, false].first
sel = flag ? :upcase : :downcase
p ["ab"].map(&sel)
