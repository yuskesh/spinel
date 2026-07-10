# A method whose return slot promoted to Bigint (an accumulator multiply under
# --int-overflow=promote) but whose early guard returns a plain int literal
# used to emit `return 0LL` through the sp_Bigint* slot -- a NULL pointer that
# segfaulted the caller's first sp_bigint_cmp (tep's proxy retry loop). The
# int value must wrap into a real bigint at the return boundary.
class RetryPolicy
  attr_accessor :base_backoff_ms, :backoff_multiplier
  def initialize
    @base_backoff_ms = 0
    @backoff_multiplier = 2
  end
  def backoff_for(attempt)
    if @base_backoff_ms <= 0
      return 0
    end
    d = @base_backoff_ms
    i = 0
    while i < attempt
      d = d * @backoff_multiplier
      i += 1
    end
    d
  end
end

class Base
  def retry_policy
    RetryPolicy.new
  end
  def handle(attempt)
    policy = retry_policy
    backoff = policy.backoff_for(attempt)
    if backoff > 0
      puts "sleep"
      puts backoff
    else
      puts "no sleep"
    end
  end
end

class Sub < Base
  def retry_policy
    p = RetryPolicy.new
    p.base_backoff_ms = 100
    p
  end
end

Base.new.handle(1)   # zero base -> the early `return 0` path
Sub.new.handle(0)    # 100 * 2**0
Sub.new.handle(3)    # 100 * 2**3
Sub.new.handle(62)   # overflows mrb_int: the promotion that types the slot
