# Array size/index operations validate their argument: a negative count to
# take/drop raises ArgumentError, and an out-of-range negative insert index
# raises IndexError. Spinel previously proceeded silently (drop even returned a
# tail slice; a too-negative insert clamped to the front).
def check
  yield
rescue => e
  puts "#{e.class}: #{e.message}"
end

def dr(a, n); a.drop(n); end
def tk(a, n); a.take(n); end
def insi(a, n); a.insert(n, 9); end
def inss(a, n); a.insert(n, "z"); end

check { dr([1, 2], -1) }        # ArgumentError: attempt to drop negative size
check { tk([1, 2], -1) }        # ArgumentError: attempt to take negative size
check { insi([1, 2], -5) }      # IndexError: index -5 too small for array; minimum: -3
check { inss(["a", "b"], -4) }  # IndexError: index -4 too small for array; minimum: -3

# The valid forms still work.
p dr([1, 2, 3], 1)              # [2, 3]
p tk([1, 2, 3], 2)              # [1, 2]
p dr([1, 2], 0)                 # [1, 2]
p insi([1, 2], -3)             # [9, 1, 2]  (minimum valid negative index)
p insi([1, 2], 1)              # [1, 9, 2]
p inss(["a", "b"], -1)         # ["a", "b", "z"]
