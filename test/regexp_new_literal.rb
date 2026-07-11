# `Regexp.new("literal")` / `Regexp.compile("literal")` with a
# string-literal argument folds into the same static pattern table
# as a `/literal/` regex literal. The LV/const holding the value
# is a placeholder; `.match?` / `.match` / `=~` resolve at the call
# site through `regex_pat_c_expr`, dispatching to `sp_re_pat_<i>`.
# Without this, the LV was typed `obj_Regexp` and codegen emitted
# `sp_Regexp *lv_x = NULL;` which doesn't compile (no such type).

# Direct call on the constructor expression.
puts Regexp.new("foo").match?("foobar")   # true
puts Regexp.new("xyz").match?("foobar")   # false

# Local variable bound to a Regexp.new result.
pat = Regexp.new("\\d+")
puts pat.match?("hello 42")                # true
puts pat =~ "abc 123"                      # 4

# Compile alias.
pat2 = Regexp.compile("foo")
puts pat2.match?("foobar")                 # true

# Constant bound to a Regexp.new result.
RE = Regexp.new("foo")
puts RE.match?("foobar")                   # true
puts "abc foo def" =~ RE                   # 4

# Dynamic-arg form: the argument is built at runtime via interpolation.
# scan_features registers the CallNode with the per-call-site
# `sp_re_dyn_<idx>` cache; at the read site `regex_pat_c_expr`
# re-evaluates the arg string and the cache returns the compiled
# pattern (recompile only on key change).
tag = "div"
puts Regexp.new("<#{tag}").match?("<div>")  # true
puts Regexp.new("<#{tag}").match?("<span>") # false

# LV bound to a dyn Regexp.new -- read site re-emits the dyn-cache
# call, so the cache lookup uses the LV's free variables (here `tag`)
# in scope at the read site.
pat_dyn = Regexp.new("<#{tag}>")
puts pat_dyn.match?("<div>")                # true
puts pat_dyn =~ "before <div> after"        # 7

# Dyn arg from method parameters -- the common idiom for an
# assert_select-style helper.
def find_tag(body, tag)
  re = Regexp.new("<#{tag}[^>]*>")
  body =~ re
end
puts find_tag("foo <span class=\"y\"> bar", "span") # 4

# Regexp.escape neutralizes regex metachars so the resulting
# string matches the original bytes literally inside `Regexp.new`.
puts Regexp.escape("foo+bar")              # foo\+bar
puts Regexp.escape("a.b*c")                # a\.b\*c
content = "foo.bar"
literal_pat = Regexp.new(Regexp.escape(content))
puts literal_pat.match?("foo.bar")         # true
puts literal_pat.match?("fooXbar")         # false (the . is now literal)

# `=~` returns Integer|nil matching CRuby (not -1 for no-match).
puts ("abc" =~ /b/).inspect                # 1
puts ("abc" =~ /xyz/).inspect              # nil
puts ("abc" =~ /xyz/).nil?                 # true
puts ("abc" =~ /b/).nil?                   # false

# $~ / $& / $` / $' -- regex match globals. $~ is a real MatchData
# (its to_s is the whole match, so `puts $~` prints like $&).
"hello world" =~ /(o w)/
puts $~                                    # o w
puts $&                                    # o w
puts $`                                    # hell
puts $'                                    # orld
