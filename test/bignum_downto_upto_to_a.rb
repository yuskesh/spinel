# Bignum#downto / #upto materialized with .to_a (KieranP #2305)
big = 2 ** 100
p big.downto(big - 3).to_a
p big.upto(big + 2).to_a
p big.downto(big - 2).size
p big.downto(big + 5).to_a           # descending past the limit -> empty
p (2 ** 64).upto(2 ** 64 + 1).to_a
