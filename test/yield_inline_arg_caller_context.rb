# An argument to a yielding (inlined) method is call-site code: it must
# resolve against the CALLER's self and class, not the receiver's. Here
# `interval` is the caller Widget's attr_reader, interpolated into the
# cache key passed to Cache#fetch. Regression: the receiver-context
# switch happened before argument binding, so `interval` resolved (and
# was invoked) against the Cache receiver instead of the Widget caller.
class Cache
  def fetch(key)
    @store ||= {}
    return @store[key] if @store.key?(key)
    @store[key] = yield
  end
end

class Widget
  def initialize(name)
    @name = name
  end

  def interval
    @name
  end

  def compute
    # `interval` (caller's reader) is in the ARG; the block is caller code too.
    Cache.new.fetch("data_#{interval}") { "value_for_#{interval}" }
  end
end

puts Widget.new("hourly").compute
puts Widget.new("daily").compute
