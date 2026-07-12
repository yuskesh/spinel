# Negative Float ** fractional exponent raises Math::DomainError loudly
# (CRuby promotes to a Complex; documented divergence in
# docs/limitations.md -- expected file pins the spinel behavior).
begin
  p((-2.0) ** 0.5)
rescue => e
  puts "#{e.class}: raised"
end
p(2.0 ** 0.5)
p((-2.0) ** 2.0)
p((-8.0) ** (1.0 / 3)) rescue puts "raised2"
