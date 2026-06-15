# Math.* methods raise Math::DomainError on out-of-domain input,
# matching CRuby semantics. Previously spinel called bare libc
# sqrt(-1.0) etc. which returns NaN silently. Math::DomainError is
# now in the known-exception list and the ancestry walker wires it
# as < StandardError < Exception, so the natural CRuby idiom works:
#
#   x = Math.sqrt(input) rescue 0.0
#   begin Math.log(x); rescue Math::DomainError => e; ...; end
#
# Methods with restricted domains (sqrt, log, log2, log10, acos,
# asin, acosh, atanh, plus Integer.sqrt) get sp_math_* wrappers.
# Unrestricted methods (cos, sin, tan, atan, sinh, cosh, tanh,
# asinh, exp, atan2, hypot) call libc directly — no overhead.

# Sanity: in-domain values still work and produce expected output.
puts Math.sqrt(4.0)
puts Math.sqrt(2.0).round(4)
puts Math.acos(1.0)
puts Math.asin(0.0)
puts Math.log(1.0)

# sqrt(negative) → Math::DomainError
begin
  Math.sqrt(-1.0)
  puts "no raise"
rescue Math::DomainError => e
  puts "sqrt: #{e}"
end

# log(negative)
begin
  Math.log(-1.0)
  puts "no raise"
rescue Math::DomainError => e
  puts "log: #{e}"
end

# log2 / log10
begin
  Math.log2(-1.0)
rescue Math::DomainError => e
  puts "log2: #{e}"
end
begin
  Math.log10(-1.0)
rescue Math::DomainError => e
  puts "log10: #{e}"
end

# acos / asin out of [-1, 1]
begin
  Math.acos(2.0)
rescue Math::DomainError => e
  puts "acos: #{e}"
end
begin
  Math.asin(-2.0)
rescue Math::DomainError => e
  puts "asin: #{e}"
end

# acosh requires x >= 1
begin
  Math.acosh(0.5)
rescue Math::DomainError => e
  puts "acosh: #{e}"
end

# atanh raises for |x| > 1; the endpoints ±1 yield ±Infinity (no raise in CRuby)
begin
  Math.atanh(2.0)
rescue Math::DomainError => e
  puts "atanh: #{e}"
end

# Integer.sqrt of negative
begin
  Integer.sqrt(-4)
rescue Math::DomainError => e
  puts "isqrt: #{e}"
end

# Hierarchy: Math::DomainError < StandardError < Exception
begin
  Math.sqrt(-1.0)
rescue StandardError => e
  puts "as Std: caught"
end
begin
  Math.sqrt(-1.0)
rescue Exception => e
  puts "as Exc: caught"
end

# Bare rescue catches it
begin
  Math.sqrt(-1.0)
rescue => e
  puts "bare-rescue: #{e}"
end

# Unrestricted methods don't raise — return NaN/Infinity per IEEE 754
puts Math.cos(0.0)
puts Math.exp(1.0).round(4)

# 2-arg Math.log(x, base): a valid base computes log_base(x); a NEGATIVE base
# raises (log of a negative). Per CRuby a base of 0.0 or 1.0 does NOT raise
# (it yields -0.0 / NaN), so only the negative-base domain error is asserted.
puts Math.log(8.0, 2.0)
begin
  Math.log(2.0, -1.0)
  puts "no raise"
rescue Math::DomainError => e
  puts "log neg base: #{e}"
end
