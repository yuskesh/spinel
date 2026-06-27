# Thread.new(arg) passes the argument as the block's first parameter -- the
# canonical way to give each thread its own value instead of capturing a shared
# variable. The argument reaches the block on the thread's first scheduling.
# (A block whose tail is a poly-typed method call -- e.g. x.upcase -- is a
# separate emit-fiber value-capture gap, so the values below are captured forms.)

t = Thread.new(21) { |n| n * 2 }
p t.value                                   # 42

p Thread.new("hi") { |x| x + "!" }.value    # "hi!"

# the classic loop: each thread gets its own i, not a shared captured one
results = (1..5).map { |i| Thread.new(i) { |n| n * n } }
p results.map(&:value)                      # [1, 4, 9, 16, 25]

# the argument captured by value, observed through a side effect
seen = []
Thread.new(:tag) { |n| seen << n }.join
p seen                                      # [:tag]

# no argument -> the param is nil
p Thread.new { |n| n.inspect }.value        # "nil"
