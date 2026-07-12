# A yielded block binds keyword params by name from the yielded kwargs hash;
# an omitted optional keyword takes its default.
def kw; yield(a: 1, b: 2); end
def kwd; yield(x: 5); end
def mixed; yield(1, k: 9); end
kw { |a:, b:| p a + b }
kwd { |x:, y: 10| p [x, y] }
mixed { |n, k:| p [n, k] }
