# Random.new auto-seeding (two instances differ), Random#rand(Range), and
# Base64.urlsafe_encode64's padding: keyword.
require "base64"

a = Random.new
b = Random.new
p(a.rand(1_000_000) != b.rand(1_000_000))   # true

def rr(r); Random.new(5).rand(r); end
v = rr(1..6)
p v.class                                    # Integer
p v >= 1 && v <= 6                           # true
w = rr(10...12)
p w >= 10 && w <= 11                         # true
# a seeded instance is reproducible
p Random.new(7).rand(1000) == Random.new(7).rand(1000)  # true
begin
  rr(6..1)
rescue ArgumentError => e
  puts "#{e.class}: #{e.message}"
end

def enc(x); Base64.urlsafe_encode64(x, padding: false); end
def encp(x); Base64.urlsafe_encode64(x); end
p enc("1")          # "MQ"
p encp("1")         # "MQ=="
p enc("ab")         # "YWI"
p encp("ab")        # "YWI="
p enc("abc")        # "YWJj" (no padding to strip)
