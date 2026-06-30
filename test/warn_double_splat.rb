# Kernel#warn accepts a forwarded keyword-rest (`**opts`) as its options
# bundle rather than a message. A double-splat made solely of forwarded hashes
# is evaluated for side effects and otherwise ignored; positional messages
# still reach stderr. (Literal uplevel:/category: keywords are unsupported and
# rejected at compile time, since spinel models neither.)
def s(x); x; end

opts = s({})
warn(**opts)              # forwarded empty options: no output
warn("with opts", **opts) # positional message + forwarded options
warn(**{})                # empty literal double-splat
puts "stdout done"

# the splat value is still evaluated (side effect preserved)
$count = 0
def make_opts; $count += 1; {}; end
warn(**make_opts)
p $count
