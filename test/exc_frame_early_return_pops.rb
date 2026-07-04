# An early `return` from inside begin/rescue must pop the live setjmp
# exception frames it exits. If codegen emits a bare C `return`, the frame
# stays on sp_exc_stack with a jmp_buf pointing into a dead C stack frame;
# the next raise longjmps into garbage (Doom's SoundManager#[] crashed this
# way), and repeated calls overflow the exception stack.

class SoundCache
  def initialize
    @cache = {}
  end

  def [](key)
    begin
      return @cache[key] if @cache.key?(key)
      value = key * 2
      @cache[key] = value
      value
    rescue
      -1
    end
  end
end

sc = SoundCache.new
sc[7]  # populate: the non-early path pops its frame correctly

# Each early-return hit leaked one frame (capacity is far below 200).
total = 0
1.upto(200) { |i| total += sc[7] }
puts total

# A raise must land in this fresh handler, not a stale leaked frame.
begin
  sc[7]
  raise "boom"
rescue => e
  puts "caught #{e.message}"
end

# Early return through two nested begin frames pops both.
def double_nested(flag)
  begin
    begin
      return "inner-out" if flag
      "no"
    rescue
      "r1"
    end
    "outer"
  rescue
    "r2"
  end
end

200.times { double_nested(true) }
puts double_nested(true)
begin
  raise "deep"
rescue => e
  puts "caught #{e.message}"
end

# Return from a begin/rescue nested inside begin..ensure: the deferred
# return must pop down to the frame depth at ensure entry (both the rescue
# frame and the ensure frame), then run the ensure.
def rescue_in_ensure(flag)
  begin
    begin
      return "early" if flag
      "body"
    rescue
      "rescued"
    end
  ensure
    $ensure_runs += 1
  end
end

$ensure_runs = 0
200.times { rescue_in_ensure(true) }
puts $ensure_runs
begin
  raise "after ensure"
rescue => e
  puts "caught #{e.message}"
end

# Return from a begin..ensure nested inside begin/rescue: after the ensure
# runs, the deferred return must also pop the enclosing rescue frame.
def ensure_in_rescue(flag)
  begin
    begin
      return "early2" if flag
      "body"
    ensure
      $ensure_runs2 += 1
    end
  rescue
    "rescued"
  end
end

$ensure_runs2 = 0
200.times { ensure_in_rescue(true) }
puts $ensure_runs2
begin
  raise "after ensure 2"
rescue => e
  puts "caught #{e.message}"
end
puts "done"
