# Kernel#Rational parses a String argument ("n", "n/d", "n.d", with optional
# whitespace and sign) instead of reading the string pointer as an integer, and
# raises ZeroDivisionError for a zero denominator instead of building (n/0).
def r1(s); Rational(s); end
p r1("3/4")                    # (3/4)
p r1("  -5/2 ")                # (-5/2)
p r1("6")                      # (6/1)
p r1("2.5")                    # (5/2)

def r2(a, b); Rational(a, b); end
p r2(6, 4)                     # (3/2)  (still reduces)
p r2(-1, 3)                    # (-1/3)

def check
  yield
rescue ZeroDivisionError => e
  puts "ZeroDivisionError: #{e.message}"
end
check { r2(1, 0) }             # ZeroDivisionError: divided by 0
check { r2(0, 0) }             # ZeroDivisionError: divided by 0

# The single-argument integer form is unchanged.
p Rational(3)                  # (3/1)
