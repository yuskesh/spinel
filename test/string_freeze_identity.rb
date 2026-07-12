# String#freeze marks in place and returns the SAME object:
# s.freeze.equal?(s) is true and s.frozen? flips. equal? on two string
# expressions compares pointers (identical literals share storage, like
# the frozen-string-literal world, so that case is out of scope here).
s = "hi"
p s.frozen?
p s.freeze.equal?(s)
p s.frozen?
t = "ab"
p t.equal?(t)
