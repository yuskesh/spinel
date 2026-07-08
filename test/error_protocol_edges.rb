# CRuby error-protocol edges:
# - float modulo by zero raises ZeroDivisionError (int or float divisor);
#   the divisor-sign result rule is preserved for nonzero divisors
# - Array#first(n)/#last(n) with a negative count raise ArgumentError
# - a rescue binding inside a yield-inlined method body resolves (the
#   assignment used the unrenamed local and did not compile)
def t
  yield
rescue => e
  e.class.to_s
end
p t { 5.0 % 0 }
p t { 5.0 % 0.0 }
p t { -5.5 % 2 }
p t { [1, 2, 3].first(-1) }
p t { [1, 2, 3].last(-2) }
p t { [1.0, 2.0].first(-1) }
p t { [1, 2, 3].first(2) }
p t { [1, 2, 3].last(2) }
p t { 5 % 0 }
