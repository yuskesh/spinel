# $~ answers its MatchData face (pre_match / post_match / to_s) from the same
# match registers the back-references read.
/foo/ =~ 'barfoobaz'
p $~.pre_match
p $~.post_match
p $~.to_s
p $~.pre_match == $`
p $~.post_match == $'
