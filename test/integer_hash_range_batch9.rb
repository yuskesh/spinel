# Integer quo/gcdlcm/pow/digits edges, Hash compact!/update/store/merge!/
# each_with_object(String memo), and endless Range#to_a.

# Integer
begin
  p(7.quo(0))
rescue => e
  p e.class
end
p(7.quo(2))
begin
  p(12.gcdlcm(2.5))
rescue => e
  p e.class
end
p(12.gcdlcm(8))
p(3.pow(-2))
p(2.pow(-1))
p(3.pow(2))
p((2 ** 100).digits(1024))
p(255.digits(16))

# Hash
h = { 1 => nil, 2 => "y" }
h.compact!
p(h)
h2 = { a: 1 }
h2.update({ b: 2 }, { c: 3 })
p(h2)
p({ "a" => 1, "b" => 2 }.each_with_object("") { |(k, _v), acc| acc << k })
h3 = {}
h3.store(:a, 1)
p(h3)
h4 = {}
h4.merge!({ a: 1 })
p(h4)

# endless Range#to_a raises instead of hanging
begin
  p((1..).to_a)
rescue RangeError
  puts "RangeError"
end
