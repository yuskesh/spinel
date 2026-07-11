# Symbol case conversions / succ preserve Symbol class through &:sym blocks
# and variables; String#setbyte copy-on-write on value-semantics strings;
# String#chr / #intern / #to_c.
p([:hello, :world].map(&:upcase))
p([:az].map(&:succ))
p(:hello.upcase)
s = :sym_in_var
p s.upcase
a = "hello"
a.setbyte(0, 72)
p a
p a.getbyte(0)
b = "abc"
p b.chr
p "x".intern
p "42".to_c
p "3+4i".to_c
p "1.5-2i".to_c
p "4i".to_c
