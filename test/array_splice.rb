# Array#[]= splice assignment: arr[start, len] = rhs and arr[range] = rhs
# remove `len` (or the range's span) elements at `start` and insert the RHS in
# their place, shifting the tail. The expression value is the RHS (Ruby).

def id(x) = x   # route a receiver through a param to exercise the runtime path

# --- integer arrays: start,len form ---
a = [1, 2, 3, 4, 5]
a[1, 2] = [9]            # shrink (remove 2, insert 1)
p a
a = [1, 2, 3]
a[1, 1] = [7, 8, 9]      # grow (remove 1, insert 3)
p a
a = [1, 2, 3]
a[1, 0] = [10, 11]       # insert (len 0)
p a
a = [1, 2, 3, 4]
a[1, 2] = []             # delete (empty RHS)
p a
a = [1, 2, 3]
a[1, 2] = 99             # scalar RHS -> single element
p a
a = [1, 2, 3, 4, 5]
a[-2, 1] = [8]           # negative start
p a

# --- integer arrays: range form ---
a = [1, 2, 3, 4, 5]
a[1..3] = [8, 7]
p a
a = [1, 2, 3, 4, 5]
a[1...3] = [8, 7]        # exclusive range
p a
a = [1, 2, 3, 4, 5]
a[1..-1] = [9]           # negative range end
p a
a = [1, 2, 3, 4]
a[0..1] = []             # delete via range
p a

# --- string arrays ---
s = ["a", "b", "c"]
s[1, 1] = ["x", "y"]
p s
s = ["a", "b", "c", "d"]
s[0..1] = ["z"]
p s
s = ["a", "b", "c"]
s[0, 2] = "z"            # scalar
p s

# --- float arrays ---
f = [1.0, 2.0, 3.0]
f[0, 1] = [9.5, 8.5]
p f

# --- poly arrays (heterogeneous): handle mixing + nil gaps ---
y = [1, "b", 3]
y[0, 2] = [9]
p y
y = [1, "b", 3]
y[0..1] = ["x", 9]       # mixed-type RHS
p y
y = [1, "b"]
y[4, 0] = ["z"]          # nil gap fill
p y

# --- splice an array into itself ---
a = [1, 2, 3]
a[1, 1] = a
p a

# --- through a method receiver (non-literal path) ---
p id([1, 2, 3, 4]).tap { |arr| arr[1, 2] = [7, 8, 9] }

# --- expression value is the RHS ---
a = [1, 2, 3]
x = (a[0, 1] = [9, 8])
p x

# --- poly receiver: an element of a poly array is statically poly but a typed
#     array at runtime, so these splices go through the runtime dispatch ---
a = [[1, 2, 3], "x"]
a[0][1, 1] = [9]          # same-length
p a
a = [[1, 2, 3, 4], "x"]
a[0][1, 2] = [9]          # shrink
p a
a = [[1, 2], "x"]
a[0][1, 0] = [7, 8]       # grow (insert)
p a
a = [[1, 2, 3, 4], "x"]
a[0][1..2] = [9]          # range form
p a
a = [[1, 2, 3], "x"]
a[0][0, 2] = []           # delete
p a
a = [["a", "b", "c"], 1]
a[0][0, 1] = []           # delete on a string-array poly receiver
p a
a = [[1, 2, 3], "x"]
a[0][1, 1] = 99           # scalar
p a

# --- poly receiver: expression value is the RHS ---
a = [[1, 2, 3, 4], "x"]
x = (a[0][1..2] = [9])
p x

# --- poly receiver: a value that can't stay in the typed array promotes it to a
#     poly array (boxing elements) and writes it back to the slot, matching CRuby ---
a = [[1, 2, 3], "x"]; a[0][1, 2] = nil; p a      # nil is a single element, not a delete
a = [[1, 2, 3], "x"]; a[0][1, 0] = nil; p a
a = [[1, 2, 3], "x"]; a[0][1, 1] = ["s"]; p a    # heterogeneous element
a = [[1, 2, 3], "x"]; a[0][1, 1] = "s"; p a       # heterogeneous scalar
a = [[1, 2, 3], "x"]; a[0][5, 1] = [9]; p a       # start past the end nil-fills
a = [[1.0, 2.0, 3.0], "x"]; a[0][1, 1] = ["s"]; p a
a = [["a", "b", "c"], "x"]; a[0][1, 1] = [9]; p a

# --- single-index []= promotes on an element-kind mismatch too ---
a = [[1, 2, 3], "x"]; a[0][1] = nil; p a
a = [[1, 2, 3], "x"]; a[0][1] = "s"; p a
a = [[1, 2, 3], "x"]; a[0][1] = 1.5; p a          # a Float is stored, not truncated

# --- range forms: endless / beginless / negative / nil-fill ---
a = [[1, 2, 3], "x"]; a[0][1..] = [9]; p a
a = [[1, 2, 3], "x"]; a[0][..1] = [9]; p a
a = [[1, 2, 3], "x"]; a[0][-2..-1] = [9]; p a
a = [[1, 2, 3], "x"]; a[0][1..0] = [9]; p a       # empty range inserts
a = [[1, 2, 3], "x"]; a[0][5..6] = [9]; p a       # past the end nil-fills

# --- return value is the RHS as written, even when the array is promoted ---
a = [[1, 2, 3], "x"]; p(a[0][1, 2] = nil)
a = [[1, 2, 3], "x"]; p(a[0][1] = "s")
