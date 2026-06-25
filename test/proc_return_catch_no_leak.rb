# A non-local return out of a `catch` block must restore the catch stack as it
# unwinds home, so the catch slot is not leaked. A later `throw` with no live
# catch must raise UncaughtThrowError rather than matching the stale slot and
# longjmping into the home method's freed frame.
def home
  pr = proc { return 1 }
  catch(:tag) do
    pr.call
  end
  :done
end
p home
begin
  throw :tag, 5
  puts "WRONG: no error"
rescue => e
  puts "uncaught: #{e.class}"
end
