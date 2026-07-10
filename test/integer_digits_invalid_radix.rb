# Integer#digits validates its radix: a base below 2 raises ArgumentError (a
# negative base gives "negative radix", 0 or 1 give "invalid radix N"), and a
# negative receiver raises Math::DomainError. The radix was silently coerced to
# 10 and a negative receiver silently made positive.
def d(x, b); x.digits(b); end
p d(123, 10)                       # [3, 2, 1]
p d(123, 16)                       # [11, 7]

begin; d(123, 1); rescue ArgumentError => e; puts e.message; end   # invalid radix 1
begin; d(123, 0); rescue ArgumentError => e; puts e.message; end   # invalid radix 0
begin; d(123, -5); rescue ArgumentError => e; puts e.message; end  # negative radix

def dn(x); x.digits; end
# A negative receiver raises Math::DomainError; assert the message (the class
# renders under its flattened runtime name, a separate display item).
begin; dn(-123); rescue => e; puts e.message; end                  # out of domain

# The no-arg form still defaults to base 10.
p dn(90)                           # [0, 9]
