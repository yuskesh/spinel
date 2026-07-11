# a global-variable operator assignment evaluates to the updated value --
# as a method's tail (its return value), as a call argument, and inline in
# an expression. `def tick; $count += 1; end` used to compile tick as void,
# so callers got nil (or `invalid use of void expression` in an array).

$count = 0

def tick
  $count += 1
end

p [tick, 10]
p tick
x = tick
p x

# other numeric operators, and float slots
$total = 100
p($total -= 30)
p($total *= 2)

$ratio = 1.5
p($ratio += 0.25)

# a string global: += concatenates and returns the updated string
$log = "a"
def log_append(s)
  $log += s
end

p log_append("b")
p log_append("c")
p $log

# inline in a larger expression
$n = 5
y = ($n += 3) + 2
p y
p $n
