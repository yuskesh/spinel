# Builtin exception classes as first-class values: referencing the constant
# (raise_error matchers, e.class comparisons, superclass walks) works like any
# other builtin class object; raise/rescue position already resolved by name.
# RuntimeError.superclass is StandardError (CRuby), previously Exception.
p IndexError
p FrozenError
x = ZeroDivisionError
p x
p IndexError.name
begin; raise IndexError, "m"; rescue => e; p e.class == IndexError; end
p KeyError.superclass
p FrozenError.superclass
p RuntimeError.superclass
p NotImplementedError.superclass
p FloatDomainError.ancestors.include?(RangeError)
p IndexError < StandardError
p LocalJumpError <= Exception
