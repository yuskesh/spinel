# Forwarding a **hash that carries a key not accepted by the callee's fixed
# keyword params raises ArgumentError, matching CRuby, instead of silently
# ignoring the extra key.
def take(a:); a; end
def f(h); take(**h); end

begin
  f({a: 1, b: 2})
rescue ArgumentError => e
  puts e.message          # unknown keyword: :b
end

p f({a: 5})               # 5, a valid forward still works

def two(a:, b:); [a, b]; end
def g(h); two(**h); end

begin
  g({a: 1, b: 2, c: 3, d: 4})
rescue ArgumentError => e
  puts e.message          # unknown keywords: :c, :d
end

p g({a: 1, b: 2})         # [1, 2]

# a callee WITH a keyword-rest absorbs the extras (no error).
def rest(a:, **opts); [a, opts]; end
p rest(a: 1, x: 9, y: 8)  # [1, {x: 9, y: 8}]
