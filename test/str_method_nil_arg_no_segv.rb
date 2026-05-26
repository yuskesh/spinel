# #504. Several String methods receiving the wrong arg shape used
# to SEGV. CRuby raises ArgumentError; spinel can't raise but we
# should at least not crash. Codegen emits compile_arg0 -> "0"
# (NULL) for missing args and the runtime helpers then ran
# strlen(NULL) or worse.
#
# Fix: NULL guards in sp_str_count / sp_str_delete /
# sp_str_rindex / sp_str_concat; setbyte (which would have
# written through a read-only string literal) is now warned and
# returns 0 without mutating; rindex with a regex arg routes
# through the new sp_re_rindex helper instead of the plain
# string-substring path; `<<` on a string return-types as
# `string` instead of falling through to the catch-all `int`.

p "foo".count    # CRuby: ArgumentError. spinel: 0 (was: SEGV)
p "foo".delete   # CRuby: ArgumentError. spinel: "foo" unchanged (was: SEGV)
p "foo".rindex(/missing/)  # CRuby + spinel post-#532: nil. (was: -1)
p "abcdabcd".rindex(/c/)   # CRuby & spinel: 6 (new sp_re_rindex helper)
begin
  "foo".send(:<<)          # CRuby: ArgumentError. spinel: FrozenError (after #886)
  puts "no raise"
rescue FrozenError => e
  puts "send-lshift: " + e.message
end

# setbyte on a literal: spinel adopts frozen-string-literal: true
# semantics globally, so literals raise FrozenError on mutation.
# Heap-allocated strings (via .dup, +, etc.) mutate as usual.
# Test pins both behaviors: literal -> raise, dup -> mutate.
(str = "a")
begin
  str.setbyte(0, 98)
  puts "literal not frozen: " + str
rescue FrozenError => e
  puts "frozen literal: " + e.message
end
str2 = "a".dup
str2.setbyte(0, 98)
puts str2  # "b" (heap, mutates)
