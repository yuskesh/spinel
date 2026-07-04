# In-place poly-array filters (select!/filter!/keep_if/reject!/delete_if)
# emit their loop into g_pre and used to emit the block's terminal
# condition directly into g_pre after writing `if (!`. A terminal that
# itself lowers through g_pre -- a block ending in a multi-statement
# if/else expression -- spliced its statements into the middle of the
# `if` condition, producing invalid C (doom: @projectiles.reject! whose
# block ends in `if hit ... else proj.x = new_x; ...; false end`).

items = [1, "two", 3.5, "four", 5]
moved = []
ret = items.reject! do |it|
  if it.is_a?(String)
    true
  else
    moved << it
    false
  end
end
p items
p moved
p ret.nil?          # something was removed: reject! returns self

# select! with the same terminal-if shape
nums = [1, "a", 2, "b", 3]
kept = []
nums.select! do |n|
  if n.is_a?(Integer)
    kept << n * 10
    true
  else
    false
  end
end
p nums
p kept

# nothing removed: reject! answers nil, delete_if answers self
b = [7, 8.5]
p b.reject! { |x|
  if x.is_a?(String)
    true
  else
    moved << x
    false
  end
}.nil?
p b
p moved
