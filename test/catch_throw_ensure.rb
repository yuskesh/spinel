# A non-local unwind -- a non-lambda proc's `return` and a `throw` -- runs the
# `ensure` blocks it passes over before delivering, like an exception. An
# uncaught `throw` raises a (catchable) UncaughtThrowError.

# proc `return` runs an intervening ensure, then the home method returns.
def ret_through_ensure
  begin
    proc { return 30 }.call
  ensure
    puts "ensure-ret"
  end
  40
end
p ret_through_ensure

# Two nested ensures run inner-first as the proc return unwinds.
def ret_through_two_ensures
  begin
    begin
      proc { return 1 }.call
    ensure
      puts "inner"
    end
  ensure
    puts "outer"
  end
  99
end
p ret_through_two_ensures

# A non-matching rescue is skipped; the ensure still runs.
def ret_nonmatching_rescue
  begin
    proc { return 7 }.call
  rescue TypeError
    puts "should-not-run"
  ensure
    puts "ensure-nonmatch"
  end
end
p ret_nonmatching_rescue

# A proc return that passes through no ensure goes straight home (regression).
def ret_no_ensure
  proc { return 5 }.call
  6
end
p ret_no_ensure

# proc return passing through a rescue modifier still returns home.
def ret_through_rescue_modifier
  x = (proc { return 9 }.call rescue 0)
  x + 100
end
p ret_through_rescue_modifier

# `throw` runs an intervening ensure before reaching its catch.
r = catch(:t) do
  begin
    throw :t, 11
  ensure
    puts "throw-ensure"
  end
end
p r

# A throw to an outer tag passes an inner catch of a different tag and runs the
# ensure between throw and the matching catch.
r2 = catch(:outer) do
  catch(:inner) do
    begin
      throw :outer, 22
    ensure
      puts "between-ensure"
    end
  end
  :inner_normal
end
p r2

# A throw NOT passing any ensure delivers directly (regression).
p(catch(:q) { throw :q, 33 })

# A normal exception still runs its ensure (regression).
def raise_through_ensure
  begin
    raise "boom"
  ensure
    puts "exc-ensure"
  end
end
begin
  raise_through_ensure
rescue => e
  puts "caught: #{e.message}"
end

# An uncaught throw raises UncaughtThrowError -- catchable by its class and by a
# bare rescue (it is a StandardError), with a CRuby-shaped message.
begin
  throw :nope
rescue UncaughtThrowError => e
  puts e.message
end

begin
  throw :nope, 1
rescue => e
  puts "bare: #{e.message}"
end

# An uncaught throw runs intervening ensures on its way out (it is an exception).
def uncaught_runs_ensure
  begin
    throw :gone
  ensure
    puts "uncaught-ensure"
  end
end
begin
  uncaught_runs_ensure
rescue UncaughtThrowError
  puts "rescued-uncaught"
end
