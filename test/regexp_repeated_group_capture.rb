# Imported mruby-regexp engine fix.
#
# f5cb8e139 (relocate internal jumps when copying a repeated group): a
# quantifier copies its atom's bytecode for each repetition. Copying the
# internal jump/split targets verbatim made a later copy of a grouped body
# (e.g. (a{2,3}){2}) jump back into the first copy; the overall match still
# landed right, but the capture recorded the wrong span. Each copy now relocates
# its jump/split offsets, so a repeated group keeps only its last iteration,
# like CRuby.
p(/(a{2,3}){2}/.match("aaaaa")[0])    # "aaaaa"
p(/(a{2,3}){2}/.match("aaaaa")[1])    # "aa"
p(/(ab+){2}/.match("abbab")[1])       # "ab"
p(/(\d{1,2}){3}/.match("123456")[1])  # "56"
p(/(x{2}){2}/.match("xxxx")[1])       # "xx"
