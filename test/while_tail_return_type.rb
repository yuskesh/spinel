def find(x)
  while true
    return "hit" if x > 0
    return "" if x < -10
    x += 1
  end
end
v = find(5)
puts v.upcase
def count_up(x)
  until false
    return x * 2 if x > 3
    x += 1
  end
end
puts count_up(1) + 10
def with_break(x)
  while true
    break if x > 0
  end
end
puts with_break(1).inspect
def finite(x)
  while x < 3
    return "s" if x == 2
    x += 1
  end
end
puts finite(0).inspect
puts finite(1).inspect
