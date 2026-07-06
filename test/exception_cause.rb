# Exception#cause threads the exception active when a new one is raised.
begin
  begin
    raise "inner"
  rescue
    raise "outer"
  end
rescue => e
  p e.cause.message
  p e.message
end
# A three-deep chain threads cause through each level.
begin
  begin
    begin
      raise "a"
    rescue
      raise "b"
    end
  rescue
    raise "c"
  end
rescue => e
  p e.message
  p e.cause.message
  p e.cause.cause.message
end
# An explicitly raised exception object also threads the handled exception.
begin
  begin
    raise "inner e"
  rescue
    raise RuntimeError.new("outer e")
  end
rescue => e
  p e.cause.message
  p e.message
end
# A `rescue => e` body that re-raises: the outer exception's cause chain still
# reaches through the bound exception (which carries its own cause).
begin
  begin
    begin
      raise "a"
    rescue
      raise "b"
    end
  rescue => mid
    raise "c"
  end
rescue => e
  p e.message
  p e.cause.message
  p e.cause.cause.message
end

# A non-local exit from a rescue body must not leave the handled exception as a
# stale "currently handled" state: a later unrelated raise has no cause.
def cause_after_return
  begin; raise "handled"; rescue; return 1; end
end
cause_after_return
begin; raise "fresh"; rescue => e; puts(e.cause ? "cause" : "nocause"); end

# break out of a rescue inside a loop, then an unrelated raise: no cause.
[1].each do
  begin; raise "handled"; rescue; break; end
end
begin; raise "fresh"; rescue => e; puts(e.cause ? "cause" : "nocause"); end

# next out of a rescue inside a loop, then an unrelated raise: no cause.
[1, 2].each do
  begin; raise "handled"; rescue; next; end
end
begin; raise "fresh"; rescue => e; puts(e.cause ? "cause" : "nocause"); end

# return crossing two nested rescue bodies restores the outermost saved state.
def cause_after_nested_return
  begin
    raise "outer handled"
  rescue
    begin; raise "inner handled"; rescue; return 1; end
  end
end
cause_after_nested_return
begin; raise "fresh"; rescue => e; puts(e.cause ? "cause" : "nocause"); end

# A callee that returns out of its own rescue while the caller is handling an
# exception: the caller's next raise threads the CALLER's exception as cause.
def callee_returns
  begin; raise "callee"; rescue; return 1; end
end
begin
  raise "caller"
rescue
  callee_returns
  begin
    raise "in caller"
  rescue => e
    p e.cause.message
  end
end

# return from a rescue body nested in a begin/ensure, then an unrelated raise.
def cause_after_ensure_return
  begin
    begin; raise "handled"; rescue; return 1; end
  ensure
    nil
  end
end
cause_after_ensure_return
begin; raise "fresh"; rescue => e; puts(e.cause ? "cause" : "nocause"); end

# retry out of a rescue leaves no stale state for a later unrelated raise.
$tries = 0
begin
  $tries = $tries + 1
  raise "handled"
rescue
  retry if $tries < 2
end
begin; raise "fresh"; rescue => e; puts(e.cause ? "cause" : "nocause"); end
