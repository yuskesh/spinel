# MatchData#string (the match subject), Regexp#names, and
# Regexp#named_captures ({name => [group indices]}, duplicate names
# collecting), on literal and variable receivers.
m = /(?<y>\d+)-(?<m>\d+)/.match("2026-07")
p m.string
p /(?<a>x)(?<b>y)/.names
p /(?<a>x)(?<b>y)/.named_captures
p /(?<a>x)(?<a>z)/.named_captures
p /nogroups/.names
p /nogroups/.named_captures
re = /(?<w>\w+)/
p re.names
