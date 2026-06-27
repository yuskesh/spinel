# SizedQueue on the cooperative scheduler: #push blocks when the queue is at its
# capacity until a #pop frees a slot. FIFO values are deterministic; thread
# interleaving order is not asserted.

q = SizedQueue.new(2)
p q.max                    # 2
producer = Thread.new do
  5.times { |i| q.push(i) }
  q.close
end
got = []
while (v = q.pop)
  got << v
end
producer.join
p got                      # [0, 1, 2, 3, 4]

# size / << chaining / pop on a SizedQueue with spare capacity
s = SizedQueue.new(3)
p s.empty?                 # true
s << "a" << "b"
p s.size                   # 2
p s.pop                    # "a"
p s.size                   # 1
p s.max                    # 3
