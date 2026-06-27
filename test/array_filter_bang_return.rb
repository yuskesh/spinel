# Array#select! / #filter! / #reject! return nil when nothing was removed and
# self otherwise; #keep_if / #delete_if always return self. Receivers route
# through a method param so the runtime (not a folded literal) path runs. Each
# helper is called with a single element-kind: mixing int and poly arrays
# through one param would widen the param to poly and bypass the typed path.
def si(x); x; end   # int arrays only
def sm(x); x; end   # mixed (poly) arrays only

# --- typed int array: nil-when-unchanged for the ! forms ---
p si([1, 2, 3]).reject! { |x| x > 99 }   # nothing removed -> nil
p si([1, 2, 3]).reject! { |x| x > 1 }    # removed -> self
p si([1, 2, 3]).select! { |x| x > 0 }    # all kept -> nil
p si([1, 2, 3]).select! { |x| x > 1 }    # removed -> self
p si([1, 2, 3]).filter! { |x| x.even? }  # removed -> self

# --- keep_if / delete_if always return self ---
p si([1, 2, 3]).keep_if { |x| x > 99 }   # empty, but self
p si([1, 2, 3]).delete_if { |x| x > 0 }  # empty, but self
p si([1, 2, 3]).keep_if { |x| x > 1 }    # self

# --- mutation is visible on the receiver afterwards ---
a = si([1, 2, 3, 4])
a.reject! { |x| x.even? }
p a

# --- empty receiver ---
p si([]).reject! { |x| x > 0 }           # nothing removed -> nil

# --- poly (mixed) array: same return contract ---
p sm([1, "two", 3]).reject! { |x| x == 99 }  # nothing removed -> nil
p sm([1, "two", 3]).reject! { |x| x == 3 }   # removed -> self
p sm([1, "two", 3]).select! { |x| x != 99 }  # all kept -> nil
c = sm([1, "two", 3, 4])
c.select! { |x| x != "two" }
p c

# --- frozen receiver raises FrozenError (message detail is a separate gap) ---
fi = si([1, 2, 3].freeze)
begin; fi.reject! { |x| x > 1 }; rescue => e; puts e.class; end
fm = sm(["a", 1, "b"].freeze)
begin; fm.select! { |x| x != 1 }; rescue => e; puts e.class; end
