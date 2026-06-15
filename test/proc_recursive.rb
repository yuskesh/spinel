# A proc/lambda that refers to its own binding recurses through the
# captured cell. Each body has a single recursive `.call` per expression;
# two poly-returning proc calls in one expression (e.g. naive fib) instead
# exercise the shared poly-return slot, an orthogonal limitation.
fact = proc { |n| n <= 1 ? 1 : n * fact.call(n - 1) }
p fact.call(5)

f = ->(n) { n <= 1 ? 1 : n * f.call(n - 1) }
p f.call(6)
