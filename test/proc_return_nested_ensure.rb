# A nested non-local return that itself runs an ensure -- and so completes a
# full unwind -- while executing inside an outer proc-return's ensure must not
# discard the outer return. The outer `return "outer-value"` runs its ensure,
# which calls inner_ret (whose own proc-return passes an ensure and resolves);
# the outer return value still has to be delivered.
def inner_ret
  proc {
    begin
      return 10
    ensure
      puts "  inner ensure"
    end
  }.call
  20
end
def outer
  proc {
    begin
      return "outer-value"
    ensure
      x = inner_ret
      puts "ensure ran (#{x})"
    end
  }.call
  "WRONG: fell through"
end
puts outer
