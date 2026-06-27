# Mutex#synchronize releases the lock with ensure semantics: on normal exit, on
# an exception raised in the block (re-raised to the caller), on a `return` out
# of the block, and on a non-local unwind (throw) passing through it.
m = Mutex.new

# normal completion: the block's value is returned, lock released
puts m.synchronize { 1 + 2 }
puts m.locked?

# exception in the block: re-raised, lock released
begin
  m.synchronize { raise "boom" }
rescue => e
  puts "rescued: #{e.message}"
end
puts m.locked?

# throw unwinding through synchronize: lock released
res = catch(:done) { m.synchronize { throw :done, 42 } }
puts "caught: #{res}"
puts m.locked?

# return out of the block: lock released, value returned from the method
def grab(mx)
  mx.synchronize { return 99 }
end
puts grab(m)
puts m.locked?

# nested synchronize on two mutexes
a = Mutex.new
b = Mutex.new
a.synchronize { b.synchronize { puts "nested ok" } }
puts a.locked?
puts b.locked?
