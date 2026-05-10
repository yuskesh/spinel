# Issue #414. `Time#iso8601` and `Time#strftime` were unresolved on
# a Time receiver -- spinel emitted a `cannot resolve call to ...
# on time (emitting 0)` warning and the result fell back to int.
# Downstream `.length` / string concat then either cascaded more
# (emitting 0) warnings or surfaced as a C-compile error at any
# typed-string sink.
#
# Fix:
#   - Runtime: sp_time_iso8601(t) and sp_time_strftime(t, fmt) in
#     lib/sp_runtime.h, both delegating to libc strftime against
#     the local-time broken-down value. iso8601 splices a colon
#     into the %z offset to match CRuby's "+HH:MM" form.
#   - Inference: infer_method_name_type returns "string" when
#     mname is "iso8601" / "strftime" AND recv is a Time.
#   - Codegen: compile_object_method_expr's `recv_type == "time"`
#     arm dispatches to the new runtime helpers.
#
# Coverage: shape rather than exact-string assertions, since the
# output depends on the system clock + local-time offset. We
# assert that both calls return a string of plausible length and
# that strftime honours its format (a `%Y` slot produces a
# 4-digit year).

t = Time.now
iso = t.iso8601

# iso8601 produces "YYYY-MM-DDTHH:MM:SS+HH:MM" or "...-HH:MM" or "...Z".
# A correctly-typed string has length 25 (offset form) or 20 (Z form).
puts iso.length >= 19 && iso.length <= 32 ? "iso-len-ok" : "iso-len-bad"

# The 5th character is "-" (year separator).
puts iso[4] == "-" ? "iso-shape-ok" : "iso-shape-bad"

# strftime: a fixed-format that survives clock variation.
ymd = t.strftime("%Y-%m-%d")
puts ymd.length == 10 ? "ymd-len-ok" : "ymd-len-bad"
puts ymd[4] == "-" && ymd[7] == "-" ? "ymd-shape-ok" : "ymd-shape-bad"

# strftime year-only: 4 digits.
yr = t.strftime("%Y")
puts yr.length == 4 ? "yr-ok" : "yr-bad"
