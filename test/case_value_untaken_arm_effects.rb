# case-as-value: an untaken arm's value construction must not run. The arm
# values here need setup statements (array literals, a raising call as an
# element), which used to hoist ahead of the branch dispatch and fire
# unconditionally.

def bad
  raise "boom"
end

def pick(kind)
  case kind
  when "A" then [1, 0]
  when "B" then [2, bad]
  end
end

p pick("A")

# side effects in the TAKEN arm run exactly once
$count = 0
def tick
  $count += 1
  $count
end

def build(kind)
  case kind
  when :a then [tick, 10]
  when :b then [tick, 20]
  else [tick, 30]
  end
end

p build(:b)
p $count
p build(:x)
p $count

# int scrutinee with literal labels (the C-switch fast path) scopes arm
# construction the same way
def by_num(n)
  case n
  when 1 then [1, bad]
  when 2 then ["two", 2]
  else [0, bad]
  end
end

p by_num(2)

# the raising arm still raises when it IS taken
begin
  pick("B")
rescue RuntimeError => e
  puts e.message
end
