# Thread-local storage (Thread#[] / #[]= / #key?, symbol keys), #status, and
# Thread.main on the cooperative scheduler. Each thread has its own storage.

Thread.current[:user] = "alice"
p Thread.current[:user]          # "alice"
p Thread.current[:missing]       # nil
p Thread.current.key?(:user)     # true
p Thread.current.key?(:nope)     # false

# each thread gets its own storage
t = Thread.new do
  Thread.current[:user] = "bob"
  Thread.current[:user]
end
p t.value                        # "bob"
p Thread.current[:user]          # "alice" (the main thread's slot is unaffected)

# #status
p Thread.current.status          # "run"
done = Thread.new { 1 + 1 }
done.join
p done.status                    # false (finished normally)

# Thread.main / identity ==
p Thread.main == Thread.current  # true
p Thread.new { 1 } != Thread.current  # true (distinct instances)
