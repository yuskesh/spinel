# Multi-assign of nil (a boxed sp_RbVal temp) into a captured TY_PROC cell must
# launder to NULL, not cast the sp_RbVal struct to (mrb_int) (invalid C).
def make
  p = proc { "orig" }
  esc = lambda { p ? p.call : "was nil" }   # captures p -> heap cell
  p, n = nil, 5                             # nil into the captured proc cell
  puts n
  esc
end
e = make
puts e.call
