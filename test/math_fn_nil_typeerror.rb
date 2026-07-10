# Math.<fn> accepts only a real Numeric: nil, a String, a Symbol, or any other
# non-numeric argument raises TypeError (Math does not parse strings). Spinel
# previously coerced nil to 0.0 and a String cast was a C compile error.
def ms(x); Math.sqrt(x); end

[nil, :sym, "2", [1]].each do |v|
  begin; ms(v); rescue => e; puts "#{e.class}: #{e.message}"; end
end

# valid numeric inputs (Integer and Float) still work across the fn family.
p Math.sqrt(9)          # 3.0
p Math.sqrt(2.0)        # 1.4142135623730951
p Math.log(1.0)         # 0.0
p Math.hypot(3, 4)      # 5.0
p Math.log2(8)          # 3.0

def ml(x); Math.log(x); end
begin; ml(nil); rescue => e; puts "#{e.class}: #{e.message}"; end
