# Issue #645: `slash = p.rindex("/")` followed by a nil-check
# ternary used to leak sp_RbVal into sp_str_sub_range_r (which
# expects mrb_int):
#
#   tail = slash == nil ? p : p[(slash + 1)..(p.length - 1)]
#
# Root cause: spinel widened `String#rindex` return to "poly"
# unconditionally (sp_str_rindex_poly returns sp_RbVal), even
# when the arg was a plain string. `slash + 1` then went through
# sp_poly_add and the result couldn't pass as the mrb_int start
# arg.
#
# Fix: split rindex like index — plain-string arg returns int?
# via sp_str_rindex_opt (SP_INT_NIL sentinel), regex arg stays
# on sp_re_rindex_poly. The nil-check narrow (#645 infra) then
# applies.

p = "abc/def"
slash = p.rindex("/")
tail = slash == nil ? p : p[(slash + 1)..(p.length - 1)]
puts tail

# not-found case
p2 = "no_slash_here"
slash2 = p2.rindex("/")
tail2 = slash2 == nil ? p2 : p2[(slash2 + 1)..(p2.length - 1)]
puts tail2

# multiple separators — rindex finds the last
p3 = "a/b/c/d"
puts p3.rindex("/").inspect
slash3 = p3.rindex("/")
puts slash3 == nil ? "none" : p3[(slash3 + 1)..(p3.length - 1)]

# zero-position match
puts "/foo".rindex("/")
