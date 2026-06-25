# Imported mruby-regexp alternation-priority fixes (leftmost-first).
#
# 4abed83f4: the linear-time Pike VM had no priority cut, so it kept the
# longest match rather than the first alternative Ruby prefers.
# 0df28ba5b: a 3+ branch SPLIT chain unwound its jump targets in reverse,
# mis-ranking the middle branches.

p(/a|ab/.match("ab")[0])              # "a"   (first alternative wins)
p(/ab|a/.match("ab")[0])              # "ab"
p(/cat|car|cart/.match("cart")[0])    # "car"
p(/foo|foobar|foob/.match("foobar")[0]) # "foo"
p(/one|two|three|four/.match("three")[0]) # "three"

# Greedy quantifiers still take the longest match (a surviving higher-priority
# thread overrides in a later step).
p(/\d+|\d/.match("123")[0])           # "123"
p(/a+|a/.match("aaa")[0])             # "aaa"
p(/(ab)+|x/.match("abab")[0])         # "abab"

# Captures across an alternation stay correct.
p(/(foo)|(bar)/.match("bar").captures)   # [nil, "bar"]
p(/(a)(b)|(c)/.match("ab").captures)     # ["a", "b", nil]

# Alternation inside a group, with a suffix that forces a specific branch.
p(/(cat|cats)dog/.match("catsdog")[0])   # "catsdog"
p(/(cat|cats)$/.match("cats")[1])        # "cats"

# scan over alternation
p("a1b2".scan(/[a-z]|\d/))               # ["a","1","b","2"]
