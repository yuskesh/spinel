# Named references in String#% (and the `%{name}` shorthand) read from a
# symbol-keyed hash argument. Previously rejected at compile time.
#   %<name>spec  -> the value formatted with `spec`
#   %{name}      -> the value's to_s
def s(x); x; end

p("%<n>d" % {n: 9})                      # "9"
p("%<x>05.2f" % {x: 3.14159})            # "03.14"
p("%<a>s-%<b>s" % {a: "hi", b: "yo"})    # "hi-yo"
p("%{n}" % {n: 5})                       # "5"
p("%{greeting}, world" % {greeting: "Hello"})  # "Hello, world"

# Named and brace forms mixed, with literal text and a literal percent.
p("[%<k>d] %{v}!" % {k: 3, v: "x"})      # "[3] x!"
p("100%% as %<n>d" % {n: 7})             # "100% as 7"

# Routed through a method (defeats constant-folding).
p("%<n>d" % s({n: 42}))                  # "42"

# A hash held in a local works too.
h = {label: "qty", amount: 12}
p("%<label>s=%<amount>d" % h)            # "qty=12"
