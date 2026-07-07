# throw from inside `loop` passes through loop's StopIteration handler (a
# non-local unwind in transit, like every begin/rescue passes it through);
# a break-less loop as the catch body tail types as nil and rides the slot.
i = 0
catch(:done) do
  loop do
    i += 1
    throw :done if i > 4
  end
end
p i
r = catch(:x) do
  loop do
    throw :x, 42
  end
end
p r
