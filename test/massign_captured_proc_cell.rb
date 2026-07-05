def make
  p = proc { "orig" }
  esc = lambda { p.call }   # captures p by reference -> p lives in a heap cell
  q = proc { "new" }
  p, n = q, 5               # multi-assign into the captured proc local
  puts n
  esc
end
e = make
puts e.call                 # reads the reassigned p through the closure
