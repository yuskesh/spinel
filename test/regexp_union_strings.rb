# Regexp.union builds an alternation of its operands. A String operand is
# regexp-escaped; a (flagless) Regexp operand contributes its source; a single
# Array argument is expanded into its elements; a single Regexp operand is
# returned unchanged. Operands are routed through a method param where possible
# to exercise the runtime build path too.
def id(x); x; end

# string operands, escaped
p Regexp.union("a", "b").match?("xby")
p Regexp.union("a", "b").match?("zz")
p Regexp.union("a.c").match?("a.c")
p Regexp.union("a.c").match?("abc")

# regex operands (flagless) and mixed string/regex
p Regexp.union(/a/, /b/).match?("zb")
p Regexp.union(/foo/, "ba.r").match?("ba.r")
p Regexp.union(/foo/, "ba.r").match?("baXr")

# a constant bound to a regex literal resolves to its source
PAT = /pp/
p Regexp.union(PAT, "q").match?("ppx")

# a single Array argument supplies the operands
p Regexp.union(["x", "y"]).match?("zy")

# a single operand: string is escaped, regex is returned unchanged (flags kept)
p Regexp.union("only").match?("only")
p Regexp.union(/sing/i).match?("SING")

# usable as a normal Regexp (=~, non-literal string operand)
p(Regexp.union("foo", "bar") =~ "say bar now")
p Regexp.union(id("x"), "y").match?("zy")

# empty union -> a pattern that never matches (CRuby's /(?!)/)
p Regexp.union().match?("")
p Regexp.union([]).match?("")
