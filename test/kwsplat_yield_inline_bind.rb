# A yielding method called with a forwarded **hash: its keyword params must
# bind from the splatted hash (by runtime lookup) when the inlined body yields
# to the caller's block, not fall through to a fabricated default.
def add(a:, b:); yield(a + b); end
def run_add(h); add(**h) { |s| s * 10 }; end
p run_add({a: 1, b: 2})

# a keyword param with a default, partially covered by the splat
def sub(x:, y: 100); yield(x - y); end
def run_sub(h); sub(**h) { |s| s.abs }; end
p run_sub({x: 5})
p run_sub({x: 5, y: 3})

# mixed literal keyword and forwarded splat
def prod(a:, b:); yield(a * b); end
def run_prod(h); prod(a: 7, **h) { |s| s + 1 }; end
p run_prod({b: 6})

# string-valued keywords through the splat, interpolated in the yield
def label(name:, n:); yield("#{name}=#{n}"); end
def run_label(h); label(**h) { |s| s.upcase }; end
p run_label({name: "x", n: 3})
