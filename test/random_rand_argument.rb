# Random#rand validates its bound and preserves a Float bound's return type. A
# non-positive bound (Integer or Float) silently returned 0; MRI raises
# ArgumentError "invalid argument - <n>". A Float bound was truncated to an
# integer bound and dispatched to the integer path, losing the Float return.
def ri(x); Random.new(5).rand(x); end
begin; ri(0); rescue ArgumentError => e; puts e.message; end       # invalid argument - 0
begin; ri(-12); rescue ArgumentError => e; puts e.message; end     # invalid argument - -12

def rf(x); Random.new(5).rand(x); end
begin; rf(-1.5); rescue ArgumentError => e; puts e.message; end    # invalid argument - -1.5

# A Float bound returns a Float in [0, bound); an Integer bound returns an Integer.
def clsf(x); Random.new(5).rand(x).class; end
p clsf(20.43)                       # Float
def clsi(x); Random.new(5).rand(x).class; end
p clsi(5)                           # Integer

def inrange(x); v = Random.new(5).rand(x); v >= 0.0 && v < 20.43; end
p inrange(20.43)                    # true
def ltbound(x); Random.new(5).rand(x) < 10; end
p ltbound(10)                       # true
