# %s calls to_s on every argument kind (it printed empty for symbols,
# collections, booleans, and user objects); %f prints non-finite floats
# with Ruby's casing (Inf/-Inf/NaN, where C printf lowercases).
puts format("%s", :sym)
puts format("%s", [1, 2])
puts format("%s", true)
puts format("%s|", nil)
puts format("%5s|", :ab)
class FmtC
  def to_s = "X"
end
puts format("%s", FmtC.new)
puts format("%.3f", Float::INFINITY)
puts format("%f", -Float::INFINITY)
puts format("%.2f", Float::NAN)
puts format("%s", 42)
