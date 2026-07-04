# break/next leaving a begin/rescue inside a loop must pop the live setjmp
# frame, exactly like an early return: the stale jmp_buf otherwise points
# into a dead C stack frame and the next raise longjmps into garbage
# (repeated iterations also overflow the 64-slot exception stack).

def scan(arr)
  r = 0
  i = 0
  while i < arr.length
    begin
      if arr[i] == 9
        r = i
        break
      end
      raise "odd" if arr[i].odd?
    rescue
      r = r + 100
    end
    i = i + 1
  end
  r
end

def skip_odds(arr)
  total = 0
  arr.each do |x|
    begin
      next if x.odd?
      total = total + x
    rescue
      total = total + 1000
    end
  end
  total
end

def first_big(arr)
  found = arr.each do |x|
    begin
      break x if x > 10
    rescue
      nil
    end
  end
  found
end

200.times do
  scan([2, 9, 4])
  skip_odds([1, 2, 3, 4])
  first_big([5, 20, 7])
end

p scan([2, 9, 4])
p scan([1, 2, 9])
p skip_odds([1, 2, 3, 4])
p first_big([5, 20, 7])
begin
  raise "after"
rescue => e
  puts e.message
end
