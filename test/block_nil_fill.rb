# An unfilled int-typed block/proc param holds the SP_INT_NIL sentinel.
# Collected into an array and inspected it must render as nil, not the
# raw INT64_MIN sentinel.

# Trailing param left unfilled -> nil inside the returned array.
p proc { |a, b| [a, b] }.call(1)

# A bare unfilled param still renders nil on its own (don't regress).
p proc { |a, b| a }.call(1)

# Multiple unfilled params.
p proc { |a, b, c| [a, b, c] }.call(7)

# Block passed through a method param so the call is not constant-folded.
def run(&blk) = blk.call(1)
p run { |a, b| [a, b] }
