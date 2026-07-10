# Hash#fetch's KeyError must carry the missing key: "key not found: <key.inspect>".
# The suffix was dropped, so the message was a bare "key not found". Each key type
# inspects differently (:sym, "str", 99), so the key is boxed and inspected.
def fs(h, k); h.fetch(k); end
begin; fs({a: 1, b: 2}, :zzz); rescue KeyError => e; puts e.message; end   # key not found: :zzz

def fstr(h, k); h.fetch(k); end
begin; fstr({"x" => 1}, "yy"); rescue KeyError => e; puts e.message; end   # key not found: "yy"

def fi(h, k); h.fetch(k); end
begin; fi({1 => "a"}, 99); rescue KeyError => e; puts e.message; end       # key not found: 99

# fetch_values reports the first missing key.
def fv(h, a, b); h.fetch_values(a, b); end
begin; fv({a: 1}, :a, :nope); rescue KeyError => e; puts e.message; end     # key not found: :nope

# A hash held in a poly slot (mixed value types) dispatches through the poly
# fetch path, which shared the same bare message.
def poly_h; { one: 1, two: "second" }; end
def pf(h, k); h.fetch(k); end
begin; pf(poly_h, :three); rescue KeyError => e; puts e.message; end        # key not found: :three

# A present key still returns its value, and fetch with a default is unaffected.
p fs({a: 1, b: 2}, :b)                                                       # 2
def fd(h, k); h.fetch(k, -1); end
p fd({a: 1}, :missing)                                                       # -1
