# Issue #743. `"<huge>".to_i` used to overflow int64 with undefined
# behavior and produce a garbage value. Now overflow is detected and
# saturates at INT64_MAX (closest int approximation; CRuby returns
# a bignum but spinel's int model is int64-only).

# Below int64 limit: unchanged.
puts "12345".to_i
puts "-67890".to_i

# At/over int64 max: saturates to INT64_MAX (positive) or INT64_MIN
# (negative).
puts "99999999999999999999999999999".to_i
puts "-99999999999999999999999999999".to_i

# With underscores between digits: still works, still saturates.
puts "9_223_372_036_854_775_807".to_i        # exactly INT64_MAX
puts "9_223_372_036_854_775_808".to_i        # one past, saturates
