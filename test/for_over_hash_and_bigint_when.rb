# for over a hash iterates [key, value] pairs in insertion order; a Bignum
# scrutinee matches a Bignum when-literal by value.
k = 0
l = 0
for i, j in { 1 => 10, 2 => 20 }
  k += i
  l += j
end
p k
p l
for pair in { "a" => 1 }
  p pair
end
huge = 1267650600228229401496703205376
case huge
when 1267650600228229401496703205376 then puts "big"
else puts "no"
end
