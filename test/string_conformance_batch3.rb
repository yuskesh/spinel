# String#+ non-String -> TypeError; concat no-args -> self; match non-match
# .class -> NilClass; frozen in-place bang -> FrozenError.
p("ab".concat)
begin; "a" + 1; rescue TypeError => e; puts e.message; end
begin; "a" + nil; rescue TypeError => e; puts e.message; end
begin; "a" + true; rescue TypeError => e; puts e.message; end
p("a" + "b")
p("hi".match(/z/).class)
p("hi".match(/h/).class)
s = "hi".freeze
begin; s.upcase!; rescue FrozenError => e; puts "upcase!:#{e.class}"; end
begin; s.sub!("h", "x"); rescue FrozenError => e; puts "sub!:#{e.class}"; end
p s
