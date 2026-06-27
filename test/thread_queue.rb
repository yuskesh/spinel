# Queue producer/consumer on the cooperative scheduler (Phase 0). A blocking
# #pop parks the caller until a #push hands it a value; #close drains remaining
# poppers with nil. Results are FIFO (deterministic); thread interleaving order
# is timing-dependent in CRuby, so only values/counts are asserted.

# a producer thread; the main thread consumes and blocks on the empty queue
q = Queue.new
prod = Thread.new do
  5.times { |i| q.push(i * 10) }
  q.close
end
got = []
while (v = q.pop)
  got << v
end
prod.join
p got                 # [0, 10, 20, 30, 40]
p q.closed?           # true

# a consumer thread receives every pushed value, in FIFO order
q2 = Queue.new
out = []
cons = Thread.new do
  3.times { out << q2.pop }
end
[1, 2, 3].each { |x| q2.push(x) }
cons.join
p out                 # [1, 2, 3]

# size / empty? / closed? / << chaining
q3 = Queue.new
p q3.empty?           # true
q3 << "a" << "b" << "c"
p q3.size             # 3
p q3.empty?           # false
p q3.pop              # "a"
p q3.size             # 2
