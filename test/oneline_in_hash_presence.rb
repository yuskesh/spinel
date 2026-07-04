# A bare `k:` in a one-line hash pattern is a presence-only test (no value
# constraint); a `k: Class` check still constrains the value.
def s(x); x; end
h = s({a: 1, b: 2})
p (h in {a:})
p (h in {a:, b:})
p (h in {c:})
p (h in {a: Integer, b: Integer})
p (h in {a: String})
