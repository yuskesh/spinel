# $VERBOSE / $DEBUG are predefined globals: they read false by default and
# round-trip through a save/restore (mspec's warning-suppression idiom).
p $VERBOSE
p $DEBUG
old, $VERBOSE = $VERBOSE, nil
p old
p $VERBOSE
$VERBOSE = old
p $VERBOSE
$VERBOSE = true
p $VERBOSE
