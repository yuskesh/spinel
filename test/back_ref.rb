# BackReferenceReadNode -- `$&`, `$~`, `$'`, $`, `$+`.
#
# Special globals populated by regex matches. Spinel's regex engine
# already populates sp_re_match* state for $1..$9 (NumberedReferenceReadNode);
# this extends to the symbolic back-references.
#
#   $&  -- the entire matched substring
#   $~  -- the MatchData object (aliased to $& in Spinel; no MatchData wrapper)
#   $`  -- the substring before the match
#   $'  -- the substring after the match
#   $+  -- the contents of the last group matched

"hello world" =~ /lo wo/
puts $&     # lo wo
puts $`     # hel
puts $'     # rld

# Re-match updates the state
"abcdef" =~ /cd/
puts $&     # cd
puts $`     # ab
puts $'     # ef

# $+ -- last group matched. For a multi-group pattern, $+ holds the
# highest-indexed group that successfully participated.
"abc123" =~ /([a-z]+)(\d+)/
puts $1     # abc
puts $2     # 123
puts $+     # 123

# When the last optional group doesn't participate, $+ steps back to
# the previous successful group.
"abc" =~ /([a-z]+)(\d+)?/
puts $1     # abc
puts $+     # abc
