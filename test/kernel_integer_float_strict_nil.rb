# Kernel#Integer / Kernel#Float are STRICT: nil or a non-numeric object raises
# TypeError, an unparseable String raises ArgumentError. They must not coerce
# nil to 0 the way the lenient nil.to_i / nil.to_f do.
def i(x); Integer(x); end
def fl(x); Float(x); end

[nil, :sym, true, [1]].each do |v|
  begin; i(v); rescue => e; puts "#{e.class}: #{e.message}"; end
end
begin; i("12abc"); rescue => e; puts "#{e.class}: #{e.message}"; end

# valid inputs unchanged
p i(3.9)      # 3 (truncates)
p i("42")     # 42
p i(7)        # 7

[nil, :sym].each do |v|
  begin; fl(v); rescue => e; puts "#{e.class}: #{e.message}"; end
end
begin; fl("x"); rescue => e; puts "#{e.class}: #{e.message}"; end

p fl(5)       # 5.0
p fl("3.5")   # 3.5

# lenient nil.to_i / nil.to_f are unaffected (still 0 / 0.0)
p nil.to_i    # 0
p nil.to_f    # 0.0
