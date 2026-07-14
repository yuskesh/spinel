# String#crypt — real libc crypt(3) (traditional DES), matching MRI:
# 13-char output whose first 2 chars are the salt.

r = "hello".crypt("ab")
puts r
puts r.length
puts r[0, 2]

# Determinism: same inputs → identical output.
puts "hello".crypt("ab") == "hello".crypt("ab")

# Different password → different hash, same salt prefix.
a = "hello".crypt("ab")
b = "world".crypt("ab")
puts b
puts a[0, 2] == b[0, 2]   # same salt
puts a == b                # different hash

# A salt shorter than 2 characters is an ArgumentError.
begin
  "x".crypt("z")
rescue ArgumentError => e
  puts "salt too short"
end
