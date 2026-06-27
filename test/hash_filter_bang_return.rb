# Hash#select! / #filter! / #reject! return nil when nothing was removed and
# self otherwise; #keep_if / #delete_if always return self. These now work in
# expression position (as an argument to `p`), not only as a bare statement.
# Each helper takes a single hash variant so the param type stays concrete.
def hs(x); x; end   # sym-keyed hashes
def hz(x); x; end   # str-keyed hashes

# --- sym-keyed hash: nil-when-unchanged for the ! forms ---
p hs({a: 1, b: 2}).reject! { |k, v| v > 99 }  # nothing removed -> nil
p hs({a: 1, b: 2}).reject! { |k, v| v > 1 }   # removed -> self
p hs({a: 1, b: 2}).select! { |k, v| v > 0 }   # all kept -> nil
p hs({a: 1, b: 2}).select! { |k, v| v > 1 }   # removed -> self
p hs({a: 1, b: 2}).filter! { |k, v| v == 1 }  # removed -> self

# --- keep_if / delete_if always return self ---
p hs({a: 1, b: 2}).keep_if { |k, v| v > 99 }  # empty, but self
p hs({a: 1, b: 2}).delete_if { |k, v| v > 0 } # empty, but self

# --- str-keyed hash ---
p hz({"x" => 10, "y" => 20}).reject! { |k, v| v > 15 }  # removed -> self
p hz({"x" => 10, "y" => 20}).reject! { |k, v| v > 99 }  # nothing removed -> nil

# --- mutation is visible on the receiver afterwards ---
h = hs({a: 1, b: 2, c: 3})
h.select! { |k, v| v.odd? }
p h

# --- empty receiver ---
p hs({}).reject! { |k, v| true }   # nothing removed -> nil

# --- frozen receiver raises FrozenError (message detail is a separate gap) ---
fh = hs({a: 1, b: 2}.freeze)
begin; fh.reject! { |k, v| v > 1 }; rescue => e; puts e.class; end
