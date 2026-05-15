# Stress test: lambda parameter inference through a chain of top-level
# methods. infer_lambda_param_types runs a fixed-point loop that
# discovers `g`-as-lambda one hop at a time: f1's g is detected from
# the direct `.call`, then f2 inherits because it passes its g to f1
# (which now has lambda-typed param), then f3 inherits from f2, etc.
# A chain of N methods needs N iterations to converge. With the new
# 256-iteration safety cap and oscillation guard, this monotonic
# 5-deep chain must converge cleanly without tripping either.

def f1(g)
  g.call(1)
end

def f2(g)
  f1(g)
end

def f3(g)
  f2(g)
end

def f4(g)
  f3(g)
end

def f5(g)
  f4(g)
end

double = ->(x) { x * 2 }
puts f5(double)
puts f4(double)
puts f3(double)
puts f2(double)
puts f1(double)
