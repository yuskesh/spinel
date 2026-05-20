# CRuby's `String#to_i` accepts `_` between consecutive digits and
# stops at the first non-digit. spinel previously emitted
# `(mrb_int)atoll(s)` which stops at the first `_`, returning 1
# for "1_2_3asdf" instead of 123. Now routes through
# sp_str_to_i_cruby. Issue #619 puzzle 1.

p("1_2_3asdf".to_i)
p("-5_0".to_i)
p("hello".to_i)
p("  42  trailing".to_i)
p("".to_i)
p("0xFF".to_i)   # CRuby: 0 (only base-10 digits)
