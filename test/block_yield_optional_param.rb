# A yielded block binds optional positional params to the yielded arg when
# present, else to the declared default expression.
def one; yield 5; end
def two; yield 5, 99; end
one { |a, b=10| p [a, b] }
one { |a, b=10, c=20| p [a, b, c] }
two { |a, b=10| p [a, b] }
def with_arg(d); yield d; end
with_arg(3) { |a, b=(a + 2)| p [a, b] }
