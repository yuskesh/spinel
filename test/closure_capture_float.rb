# A closure that captures a float-typed outer variable: the capture rides a
# native double cell, so reads, writes, and compound-assignment all work. (A
# proc that also takes a float parameter is still rejected, since float proc
# arguments ride the truncating int slot.)

# mutate a captured float accumulator across several calls
def running_total
  total = 0.0
  add_half = -> { total += 0.5 }
  add_half.call
  add_half.call
  add_half.call
  total
end
puts running_total

# read-only capture of a float
def scaled
  factor = 2.5
  scale = -> { factor * 10.0 }
  scale.call
end
puts scaled

# capture mutated inside the proc, then read back in the enclosing scope
def accumulate
  acc = 1.0
  bump = -> { acc *= 2.0 }
  bump.call
  bump.call
  acc
end
puts accumulate

# int and float captures coexisting in one proc
def mixed
  n = 0
  sum = 0.0
  step = -> { n += 1; sum += 1.5 }
  step.call
  step.call
  "#{n}/#{sum}"
end
puts mixed
