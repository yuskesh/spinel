# Thread.new runs a green thread on the cooperative scheduler (Phase 0, N=1):
# #value returns the block result, #join blocks until it finishes, #alive?
# reports completion, an unhandled exception is re-raised at #join/#value, and
# threads carried in an array dispatch #value through the poly slot.
# (docs/thread-mn-design.md)

t = Thread.new { 1 + 2 }
puts t.value                                  # 3
puts Thread.new { "hi".upcase }.value         # HI

# join returns the thread; value is idempotent after it finishes
u = Thread.new { 40 + 2 }
u.join
puts u.value                                  # 42
puts u.alive?                                 # false

# a side effect is observed after join
log = []
Thread.new { log << :ran }.join
puts log.inspect                              # [:ran]

# an array of threads, each value collected through the poly slot
threads = (1..3).map { |i| Thread.new { i * i } }
puts threads.map { |th| th.value }.inspect    # [1, 4, 9]

# an unhandled exception is re-raised in the joining thread (silence the
# at-termination report so the test asserts no stderr)
Thread.report_on_exception = false
begin
  Thread.new { raise "boom" }.value
rescue => e
  puts "#{e.class}: #{e.message}"             # RuntimeError: boom
end

# the main thread is itself a live Thread
puts Thread.current.alive?                     # true
