# Integer#to_f on a value beyond 63 bits. The conversion narrowed the bignum to
# a low 64-bit limb before the Float cast, so anything past int64 read as 0.0 or
# garbage. It now folds every limb into the double directly.
def f(x); x.to_f; end
p f(2 ** 64)
p f(10 ** 20)
p f(1 - 2 ** 70)
p f(2 ** 100)

# The receiver is a bignum literal (folds through the same to_f arm).
p (2 ** 64).to_f
p (2 ** 70).to_f

# A bignum flowing through a poly slot (Array element) reaches the poly to_f
# helper, which had the same narrow-then-widen bug.
def g(a); a.map { |v| v.to_f }; end
p g([2 ** 64, 3, 10 ** 25])
