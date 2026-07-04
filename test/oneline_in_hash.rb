# One-line `expr in pattern` with a hash pattern and a class-checked capture.
def s(x); x; end
data = s({x: 5})
matched = (data in {x: Integer => xv}); p matched
b = (data in {x: Integer}); p b
c = (data in {x: String});  p c
d = (data in {y: Integer}); p d
