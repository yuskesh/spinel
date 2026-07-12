# Array#[start, len] / #slice(start, len) with start outside [-len, len]
# returns nil like CRuby (start == len is the empty slice; a negative
# length is nil) -- on typed int/str arrays and poly arrays, through both
# the nil-capable and the plain slice arms.
