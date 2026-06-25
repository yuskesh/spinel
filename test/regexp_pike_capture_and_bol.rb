# Imported mruby-regexp engine fixes.
#
# c0c8c7d47 (capture loss in Pike VM thread compaction): an alternation whose
# winning branch is not the first one used to come back with a nil capture,
# because the per-step compaction renumbered capture slots in place and a low
# slot was clobbered before a later thread read it.
p(/(\d)|(x)/.match("1").captures)        # ["1", nil]
p(/(a)|(b)|(c)/.match("b").captures)     # [nil, "b", nil]
p(/(a)|(b)|(c)/.match("c").captures)     # [nil, nil, "c"]
p(/(foo)|(bar)|(baz)/.match("baz")[3])   # "baz"
p("a1b2".scan(/(\d)|([a-z])/))           # [[nil,"a"],["1",nil],[nil,"b"],["2",nil]]

# 6d99e8dec (multiline ^ after final newline): a trailing '\n' does not start a
# new line, so ^ must not match at end-of-string.
p("a\n".scan(/^/).size)                  # 1
p("a\nb\n".scan(/^/).size)               # 2
p(/^$/.match?("a\n"))                    # false
p("x\n".match?(/x$/))                    # true  ($ still matches before \n)
p("a\nb".scan(/^./))                     # ["a", "b"]
