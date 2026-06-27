# Mutex on the cooperative scheduler: #synchronize wraps the block in lock/unlock,
# and lock/unlock/locked?/owned?/try_lock behave as in CRuby.

# synchronize protects a shared counter across threads (created in a map, joined
# via &:join, so a Thread carried in a poly slot dispatches #join correctly)
m = Mutex.new
counter = 0
threads = (1..5).map { Thread.new { 10.times { m.synchronize { counter += 1 } } } }
threads.each(&:join)
p counter                 # 50

# explicit lock / unlock / locked? / owned?
m2 = Mutex.new
p m2.locked?              # false
m2.lock
p m2.locked?              # true
p m2.owned?               # true
m2.unlock
p m2.locked?              # false

# try_lock acquires only when free
p m2.try_lock             # true
p m2.try_lock             # false (we already hold it)
m2.unlock

# synchronize returns the block's value
r = m2.synchronize { 7 * 6 }
p r                       # 42
