# Thread#name / #name= name a thread; the default is nil.
t = Thread.new { 1 }
p t.name                      # nil
t.name = "worker"
p t.name                      # "worker"
t.join

# the main thread can be named too
p Thread.main.name            # nil
Thread.current.name = "main-thread"
p Thread.current.name         # "main-thread"
