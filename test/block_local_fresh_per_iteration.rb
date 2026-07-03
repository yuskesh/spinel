# Block-local variables are FRESH on every block invocation: a name first
# assigned inside a block (not captured from the enclosing scope) must not
# carry its value into the next iteration. The `keep ||=` pattern below
# relied on that freshness; without the per-iteration reset every element
# after the first truthy one reused the stale value.

def widen(x); x; end

# --- each_with_index over a poly array, `||=` fallback (the doom shape) ---
items = widen([1, 60, 2, 70, 3])
out = []
items.each_with_index do |v, i|
  keep = v if v > 50
  keep ||= "none"
  out << keep
end
p out
# ["none", 60, "none", 70, "none"]

# --- each over an int array, plain conditional first-assignment ---
hits = []
[5, 25, 8, 30].each do |n|
  big = "big" if n > 20
  hits << (big ? big : "small")
end
p hits
# ["small", "big", "small", "big"]

# --- times loop, block-local shadowless accumulator probe ---
3.times do |t|
  first_time = "yes" if t == 0
  puts first_time.nil? ? "fresh" : first_time
end
# yes
# fresh
# fresh

# --- map: block-local must not leak into the next element's value ---
vals = [10, 1, 20, 2].map do |n|
  label = "high" if n >= 10
  label || "low"
end
p vals
# ["high", "low", "high", "low"]

# --- outer local of the same kind is NOT reset (captured, not block-local) ---
count = 0
[1, 2, 3].each do |n|
  count += 1
end
puts count
# 3
