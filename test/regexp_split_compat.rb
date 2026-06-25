# spinel-side String#split(regexp[, limit]) compatibility, mirroring CRuby and
# upstream mruby-regexp string_regexp.rb#split (62a55cefe split compat,
# b61e44b35 limit). Fixes: zero-width matches split into characters instead of
# emitting empty fields, trailing empty fields are stripped under the default
# limit (kept under a negative limit), an empty subject is always [], unmatched
# optional capture groups are omitted, and a positive limit caps the field
# count with the remainder as the last field.

# zero-width / empty pattern
p("abc".split(//))               # ["a", "b", "c"]
p("abc".split(//, -1))           # ["a", "b", "c", ""]
p("abc".split(//, 2))            # ["a", "bc"]
p("héllo".split(//))             # ["h", "é", "l", "l", "o"]
p("hello".split(/l/))            # ["he", "", "o"]

# trailing empties: stripped by default, kept by negative limit
p("a1b2c3".split(/(\d)/))        # ["a", "1", "b", "2", "c", "3"]
p("a1b2c3".split(/(\d)/, -1))    # ["a", "1", "b", "2", "c", "3", ""]
p("a,b,,".split(/,/))            # ["a", "b"]
p("a,b,,".split(/,/, -1))        # ["a", "b", "", ""]

# positive limit caps fields; captures still spliced
p("a1b2c3".split(/(\d)/, 2))     # ["a", "1", "b2c3"]
p("1-2-3".split(/-/, 2))         # ["1", "2-3"]

# empty subject, no match, leading empty, unmatched optional group
p("".split(/,/))                 # []
p("".split(//))                  # []
p("abc".split(/x/))              # ["abc"]
p("1a2b".split(/(\d)/))          # ["", "1", "a", "2", "b"]
p("ab".split(/(x)?/))            # ["a", "b"]
