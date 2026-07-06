# `next` leaves the BLOCK with its value. Three emission contexts: a _proc_N
# function body (next = the proc's return), a yield-inlined block splice
# (next = leave the do{}while(0) wrapper with the value), and a genuine loop
# (next = plain continue -- must stay untouched).
a = []
-> {
  a << 1
  next
  a << 2
}.call
p a
f = -> { next 42; 7 }
p f.call
def r(val)
  v = yield()
  val == v
end
p r(nil) { next }
p r(1) { next 1 }
p r(5) { 5 }
g = proc { [1, 2, 3].each { |i| next if i == 2; a << i }; a.length }
p g.call
