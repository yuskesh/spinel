# A range pattern in `case/in` matches by membership (===), honoring
# exclusivity and beginless/endless bounds, instead of being treated as an
# unconditional match (which previously selected the wrong arm). Scrutinees
# route through a method param so the runtime path runs. Separate helpers per
# scrutinee kind (int/float/poly) keep the param type concrete.
def si(x); x; end   # int
def sf(x); x; end   # float
def sp(x); x; end   # poly

def classify(n)
  case si(n)
  in 0..3 then "low"
  in 4..10 then "mid"
  in 11.. then "high"
  else "neg"
  end
end
p classify(2)    # "low"
p classify(5)    # "mid"
p classify(50)   # "high"
p classify(-1)   # "neg"

# exclusive end: 3 is NOT in 0...3
def excl(n)
  case si(n)
  in 0...3 then "lo"
  in 3..9 then "hi"
  end
end
p excl(2)        # "lo"
p excl(3)        # "hi"

# beginless / endless ranges
def beginless(n)
  case si(n)
  in ..0 then "neg"
  in 1.. then "pos"
  end
end
p beginless(-5)  # "neg"
p beginless(9)   # "pos"

# float scrutinee and bounds
def grade(x)
  case sf(x)
  in 0.0...0.5 then "F"
  in 0.5..1.0 then "P"
  end
end
p grade(0.25)    # "F"
p grade(0.75)    # "P"

# range pattern with a capture binding
case si(7)
in (5..10) => v then p v   # 7
end

# poly scrutinee (element of a poly array) still matches a range
case sp([1, "x"]).first
in 0..3 then puts "a"
in 4..9 then puts "b"
end                # a
