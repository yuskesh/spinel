# ConditionVariable on the cooperative scheduler: #wait releases the mutex and
# parks until #signal/#broadcast wakes the waiter, which re-acquires the mutex.

m = Mutex.new
cv = ConditionVariable.new
ready = false
log = []
worker = Thread.new do
  m.synchronize do
    cv.wait(m) until ready
  end
  log << :worked
end
Thread.pass                 # let the worker reach cv.wait
m.synchronize do
  ready = true
  cv.signal
end
worker.join
log << :done
p log                       # [:worked, :done]
p ready                     # true

# broadcast wakes every waiter
m3 = Mutex.new
cv3 = ConditionVariable.new
go = false
done = []
ws = (1..3).map do |i|
  Thread.new do
    m3.synchronize { cv3.wait(m3) until go }
    done << i
  end
end
Thread.pass                 # all three reach cv3.wait
m3.synchronize { go = true; cv3.broadcast }
ws.each(&:join)
p done.sort                 # [1, 2, 3]
